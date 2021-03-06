/*
 * Copyright 2014-2020 Real Logic Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(__linux__)
#define _BSD_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#if !defined(_MSC_VER)
#include <unistd.h>
#endif

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include <aekv/coroutine.h>

#include "aeronc.h"
#include "aeronmd.h"
#include "aeron_client.h"
#include "aeron_driver_context.h"
#include "concurrent/aeron_atomic.h"
#include "util/aeron_strutil.h"
#include "aeron_agent.h"

#include "samples_configuration.h"
#include "sample_util.h"

const char usage_str[] =
    "[-h][-v][-C uri][-c uri][-p prefix][-S stream-id][-s stream-id]\n"
    "    -h               help\n"
    "    -v               show version and exit\n"
    "    -C uri           use channel specified in uri for pong channel\n"
    "    -c uri           use channel specified in uri for ping channel\n"
    "    -p prefix        aeron.dir location specified as prefix\n"
    "    -S stream-id     stream-id to use for pong channel\n"
    "    -s stream-id     stream-id to use for ping channel\n";

typedef struct {
  coroutine_t *client_coro,
              *runner_coro;
  aeron_t     *aeron;
  const char  *pong_channel;
  const char  *ping_channel;
  int32_t      pong_stream_id;
  int32_t      ping_stream_id;

  aeron_async_add_subscription_t          *async_ping_sub;
  aeron_async_add_exclusive_publication_t *async_pong_pub;
  aeron_subscription_t                    *subscription;
  aeron_image_t                           *image;
  aeron_exclusive_publication_t           *publication;
  aeron_image_fragment_assembler_t        *fragment_assembler;
  struct hdr_histogram                    *histogram;
} pong_client_t;

typedef struct {
  coroutine_t    *driver_coro;
  aeron_driver_t *driver;
} driver_data_t;

volatile bool running = true;

void sigint_handler(int signal)
{
    AERON_PUT_ORDERED(running, false);
}

inline bool is_running()
{
    bool result;
    AERON_GET_VOLATILE(result, running);
    return result;
}

void termination_hook(void *state)
{
    AERON_PUT_ORDERED(running, false);
}

void coro_driver( coroutine_t *coro, driver_data_t *dr )
{
    while ( is_running() )
    {
        aeron_driver_main_do_work( dr->driver );
        coroutine_yield( coro );
    }
}

void coro_runner( coroutine_t *coro, pong_client_t *cl )
{
    aeron_agent_runner_t *runner = &cl->aeron->runner;
    runner->state = AERON_AGENT_STATE_MANUAL;
    while ( aeron_agent_is_running( runner ) )
    {
        runner->do_work( runner->agent_state );
        coroutine_yield( coro );
    }
    runner->state = AERON_AGENT_STATE_STOPPED;
}

void ping_poll_handler(void *clientd, const uint8_t *buffer, size_t length, aeron_header_t *header)
{
    pong_client_t *cl = (pong_client_t *)clientd;
    aeron_exclusive_publication_t *publication = cl->publication;
    aeron_buffer_claim_t buffer_claim;

    while (aeron_exclusive_publication_try_claim(publication, length, &buffer_claim) < 0)
    {
        coroutine_yield( cl->client_coro );
    }

    memcpy(buffer_claim.data, buffer, length);
    aeron_buffer_claim_commit(&buffer_claim);
}

void coro_client( coroutine_t *coro,  pong_client_t *cl )
{
    if (aeron_async_add_subscription(
        &cl->async_ping_sub,
        cl->aeron,
        cl->ping_channel,
        cl->ping_stream_id,
        print_available_image,
        NULL,
        print_unavailable_image,
        NULL) < 0)
    {
        fprintf(stderr, "aeron_async_add_subscription: %s\n", aeron_errmsg());
        goto cleanup;
    }

    while (NULL == cl->subscription)
    {
        if (aeron_async_add_subscription_poll(&cl->subscription, cl->async_ping_sub) < 0)
        {
            fprintf(stderr, "aeron_async_add_subscription_poll: %s\n", aeron_errmsg());
            goto cleanup;
        }

        if (!is_running())
        {
            goto cleanup;
        }

        coroutine_yield( coro );
    }

    printf("Subscription channel status %" PRIu64 "\n", aeron_subscription_channel_status(cl->subscription));

    if (aeron_async_add_exclusive_publication(
        &cl->async_pong_pub,
        cl->aeron,
        cl->pong_channel,
        cl->pong_stream_id) < 0)
    {
        fprintf(stderr, "aeron_async_add_exclusive_publication: %s\n", aeron_errmsg());
        goto cleanup;
    }

    while (NULL == cl->publication)
    {
        if (aeron_async_add_exclusive_publication_poll(&cl->publication, cl->async_pong_pub) < 0)
        {
            fprintf(stderr, "aeron_async_add_exclusive_publication_poll: %s\n", aeron_errmsg());
            goto cleanup;
        }

        if (!is_running())
        {
            goto cleanup;
        }

        coroutine_yield( coro );
    }

    printf("Publication channel status %" PRIu64 "\n", aeron_exclusive_publication_channel_status(cl->publication));

    while (!aeron_subscription_is_connected(cl->subscription))
    {
        if (!is_running())
        {
            goto cleanup;
        }

        coroutine_yield( coro );
    }

    if ((cl->image = aeron_subscription_image_at_index(cl->subscription, 0)) == NULL)
    {
        fprintf(stderr, "%s", "could not find image\n");
        goto cleanup;
    }

    if (aeron_image_fragment_assembler_create(&cl->fragment_assembler, ping_poll_handler, cl) < 0)
    {
        fprintf(stderr, "aeron_image_fragment_assembler_create: %s\n", aeron_errmsg());
        goto cleanup;
    }

    while (is_running())
    {
        int fragments_read = aeron_image_poll(
            cl->image, aeron_image_fragment_assembler_handler, cl->fragment_assembler, DEFAULT_FRAGMENT_COUNT_LIMIT);

        if (fragments_read < 0)
        {
            fprintf(stderr, "aeron_image_poll: %s\n", aeron_errmsg());
            goto cleanup;
        }

        coroutine_yield( coro );
    }

    printf("Shutting down...\n");

cleanup:
    aeron_subscription_image_release(cl->subscription, cl->image);
    coroutine_yield( coro );
    aeron_subscription_close(cl->subscription, NULL, NULL);
    coroutine_yield( coro );
    aeron_exclusive_publication_close(cl->publication, NULL, NULL);
    coroutine_yield( coro );
    aeron_image_fragment_assembler_delete(cl->fragment_assembler);
}

int main(int argc, char **argv)
{
    schedule_t             *sched      = NULL;
    aeron_context_t        *context    = NULL;
    aeron_driver_context_t *dr_context = NULL;
    const char             *aeron_dir  = NULL;
    pong_client_t           cl;
    driver_data_t           dr;
    int                     status = EXIT_FAILURE,
                            opt;
    memset( &cl, 0, sizeof( cl ) );
    memset( &dr, 0, sizeof( dr ) );
    sched          = coroutine_open();
    cl.runner_coro = coroutine_new( sched, (coroutine_func_t) coro_runner, &cl, "runner" );
    cl.client_coro = coroutine_new( sched, (coroutine_func_t) coro_client, &cl, "client" );
    dr.driver_coro = coroutine_new( sched, (coroutine_func_t) coro_driver, &dr, "driver" );

    cl.pong_channel   = DEFAULT_PONG_CHANNEL;
    cl.ping_channel   = DEFAULT_PING_CHANNEL;
    cl.pong_stream_id = DEFAULT_PONG_STREAM_ID;
    cl.ping_stream_id = DEFAULT_PING_STREAM_ID;

    while ((opt = getopt(argc, argv, "hvC:c:p:C:s:")) != -1)
    {
        switch (opt)
        {
            case 'C':
            {
                cl.pong_channel = optarg;
                break;
            }

            case 'c':
            {
                cl.ping_channel = optarg;
                break;
            }

            case 'p':
            {
                aeron_dir = optarg;
                break;
            }

            case 'S':
            {
                cl.pong_stream_id = strtoul(optarg, NULL, 0);
                break;
            }

            case 's':
            {
                cl.ping_stream_id = strtoul(optarg, NULL, 0);
                break;
            }

            case 'v':
            {
                printf("%s <%s> major %d minor %d patch %d\n",
                    argv[0], aeron_version_full(), aeron_version_major(), aeron_version_minor(), aeron_version_patch());
                exit(EXIT_SUCCESS);
            }

            case 'h':
            default:
                fprintf(stderr, "Usage: %s %s", argv[0], usage_str);
                exit(status);
        }
    }

    signal(SIGINT, sigint_handler);

    if (aeron_driver_context_init(&dr_context) < 0)
    {
        fprintf(stderr, "aeron_driver_context_init: %s\n", aeron_errmsg());
        goto cleanup;
    }

    if (NULL != aeron_dir && aeron_driver_context_set_dir(dr_context, aeron_dir) < 0)
    {
        fprintf(stderr, "aeron_context_set_dir: %s\n", aeron_errmsg());
        goto cleanup;
    }
    dr_context->threading_mode = AERON_THREADING_MODE_SHARED;

    if (aeron_driver_context_set_driver_termination_hook(dr_context, termination_hook, NULL) < 0)
    {
        fprintf(stderr, "ERROR: context set termination hook (%d) %s\n", aeron_errcode(), aeron_errmsg());
        goto cleanup;
    }

    if (aeron_driver_init(&dr.driver, dr_context) < 0)
    {
        fprintf(stderr, "ERROR: driver init (%d) %s\n", aeron_errcode(), aeron_errmsg());
        goto cleanup;
    }

    if (aeron_driver_start(dr.driver, true) < 0)
    {
        fprintf(stderr, "ERROR: driver start (%d) %s\n", aeron_errcode(), aeron_errmsg());
        goto cleanup;
    }

    coroutine_resume(dr.driver_coro);

    if (coroutine_status(dr.driver_coro) && is_running())
    {
        printf("Subscribing Ping at channel %s on Stream ID %" PRId32 "\n", cl.ping_channel, cl.ping_stream_id);
        printf("Publishing Pong at channel %s on Stream ID %" PRId32 "\n", cl.pong_channel, cl.pong_stream_id);

        if (aeron_context_init(&context) < 0)
        {
            fprintf(stderr, "aeron_context_init: %s\n", aeron_errmsg());
            goto cleanup;
        }

        if (NULL != aeron_dir && aeron_context_set_dir(context, aeron_dir) < 0)
        {
            fprintf(stderr, "aeron_context_set_dir: %s\n", aeron_errmsg());
            goto cleanup;
        }

        if (aeron_init(&cl.aeron, context) < 0)
        {
            fprintf(stderr, "aeron_init: %s\n", aeron_errmsg());
            goto cleanup;
        }

        while (coroutine_status(cl.runner_coro) && coroutine_status(cl.client_coro) && coroutine_status(dr.driver_coro))
        {
            coroutine_resume(cl.runner_coro);
            coroutine_resume(cl.client_coro);
            coroutine_resume(dr.driver_coro);
        }
    }
    else
    {
        fprintf(stderr, "coroutine_status: %s\n", aeron_errmsg());
    }
cleanup:
    aeron_close(cl.aeron);
    aeron_context_close(context);

    if (0 != aeron_driver_close(dr.driver))
    {
        fprintf(stderr, "ERROR: driver close (%d) %s\n", aeron_errcode(), aeron_errmsg());
    }

    if (0 != aeron_driver_context_close(dr_context))
    {
        fprintf(stderr, "ERROR: driver context close (%d) %s\n", aeron_errcode(), aeron_errmsg());
    }
    coroutine_close(sched);

    return status;
}

extern bool is_running();
