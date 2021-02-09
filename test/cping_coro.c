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
#include <dlfcn.h>

#if !defined(_MSC_VER)
#include <unistd.h>
#endif

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include <hdr_histogram.h>
#include <aekv/coroutine.h>

#include "aeronc.h"
#include "aeron_client.h"
#include "concurrent/aeron_atomic.h"
#include "util/aeron_strutil.h"
#include "util/aeron_parse_util.h"
#include "aeron_agent.h"

#include "samples_configuration.h"
#include "sample_util.h"

const char usage_str[] =
    "[-h][-v][-C uri][-c uri][-p prefix][-S stream-id][-s stream-id]\n"
    "    -h               help\n"
    "    -v               show version and exit\n"
    "    -C uri           use channel specified in uri for pong channel\n"
    "    -c uri           use channel specified in uri for ping channel\n"
    "    -L length        use message length of length bytes\n"
    "    -m messages      number of messages to send\n"
    "    -p prefix        aeron.dir location specified as prefix\n"
    "    -S stream-id     stream-id to use for pong channel\n"
    "    -s stream-id     stream-id to use for ping channel\n"
    "    -w messages      number of warm up messages to send\n";

#define MAX_MESSAGE_LENGTH (64 * 1024)

typedef struct {
  schedule_t *sched;
  coroutine_t *c, *r;
  aeron_t *aeron;
  const char *pong_channel;
  const char *ping_channel;
  uint64_t messages;
  uint64_t message_length;
  uint64_t warm_up_messages;
  aeron_async_add_subscription_t *async_pong_sub;
  aeron_async_add_exclusive_publication_t *async_ping_pub;
  aeron_subscription_t *subscription;
  aeron_image_t *image;
  aeron_exclusive_publication_t *publication;
  aeron_image_fragment_assembler_t *fragment_assembler;
  int32_t pong_stream_id;
  int32_t ping_stream_id;
  struct hdr_histogram *histogram;
} ping_client_t;

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

void null_handler(void *clientd, const uint8_t *buffer, size_t length, aeron_header_t *header)
{
}

void pong_measuring_handler(void *clientd, const uint8_t *buffer, size_t length, aeron_header_t *header)
{
    struct hdr_histogram *histogram = (struct hdr_histogram *)clientd;
    int64_t end_ns = aeron_nano_clock();
    int64_t start_ns;

    memcpy(&start_ns, buffer, sizeof(uint64_t));
    hdr_record_value(histogram, end_ns - start_ns);
}

void send_ping_and_receive_pong(
    ping_client_t *cl,
    coroutine_t *coro,
    aeron_fragment_handler_t fragment_handler,
    void *poll_clientd,
    uint64_t message_count )
{
    uint8_t message[MAX_MESSAGE_LENGTH];
    int64_t *timestamp = (int64_t *)message;

    memset(message, 0, sizeof(message));

    for (size_t i = 0; i < message_count && is_running(); i++)
    {
        int64_t position;

        do
        {
            *timestamp = aeron_nano_clock();
        }
        while ((position = aeron_exclusive_publication_offer(
            cl->publication, message, cl->message_length, NULL, NULL)) < 0);

        while (aeron_image_position(cl->image) < position)
        {
            while (aeron_image_poll(cl->image, fragment_handler, poll_clientd, DEFAULT_FRAGMENT_COUNT_LIMIT) <= 0)
            {
                coroutine_yield( coro );
            }
        }
    }
}

void coro_runner( coroutine_t *coro, ping_client_t *cl )
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

void coro_client( coroutine_t *coro,  ping_client_t *cl )
{
    if (aeron_async_add_subscription(
        &cl->async_pong_sub,
        cl->aeron,
        cl->pong_channel,
        cl->pong_stream_id,
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
        if (aeron_async_add_subscription_poll(&cl->subscription, cl->async_pong_sub) < 0)
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
        &cl->async_ping_pub,
        cl->aeron,
        cl->ping_channel,
        cl->ping_stream_id) < 0)
    {
        fprintf(stderr, "aeron_async_add_exclusive_publication: %s\n", aeron_errmsg());
        goto cleanup;
    }

    while (NULL == cl->publication)
    {
        if (aeron_async_add_exclusive_publication_poll(&cl->publication, cl->async_ping_pub) < 0)
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

    hdr_init(1, 10 * 1000 * 1000 * 1000LL, 3, &cl->histogram);

    if (aeron_image_fragment_assembler_create(&cl->fragment_assembler, pong_measuring_handler, cl->histogram) < 0)
    {
        fprintf(stderr, "aeron_image_fragment_assembler_create: %s\n", aeron_errmsg());
        goto cleanup;
    }

    printf("Warming up the media driver with %" PRIu64 " messages of length %" PRIu64 " bytes\n",
        cl->warm_up_messages, cl->message_length);

    int64_t start_warm_up_ns = aeron_nano_clock();

    send_ping_and_receive_pong( cl, coro, null_handler, NULL, cl->warm_up_messages );

    printf("Warm up complete in %" PRId64 "ns\n", aeron_nano_clock() - start_warm_up_ns);
    printf("Pinging %" PRIu64 " messages of length %" PRIu64 " bytes\n", cl->messages, cl->message_length);

    hdr_reset(cl->histogram);

    send_ping_and_receive_pong( cl, coro, aeron_image_fragment_assembler_handler, cl->fragment_assembler, cl->messages );

    hdr_percentiles_print(cl->histogram, stdout, 5, 1000.0, CLASSIC);
    fflush(stdout);

    printf("Shutting down...\n");

cleanup:
    coroutine_yield( coro );
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
    aeron_context_t *context = NULL;
    const char *aeron_dir = NULL;
    ping_client_t cl;
    int status = EXIT_FAILURE, opt;

    memset( &cl, 0, sizeof( cl ) );
    cl.sched = coroutine_open();
    cl.r     = coroutine_new( cl.sched, (coroutine_func_t) coro_runner, &cl, "runner" );
    cl.c     = coroutine_new( cl.sched, (coroutine_func_t) coro_client, &cl, "client" );

    cl.pong_channel     = DEFAULT_PONG_CHANNEL;
    cl.ping_channel     = DEFAULT_PING_CHANNEL;
    cl.messages         = DEFAULT_NUMBER_OF_MESSAGES;
    cl.message_length   = DEFAULT_MESSAGE_LENGTH;
    cl.warm_up_messages = DEFAULT_NUMBER_OF_WARM_UP_MESSAGES;
    cl.pong_stream_id   = DEFAULT_PONG_STREAM_ID;
    cl.ping_stream_id   = DEFAULT_PING_STREAM_ID;

    while ((opt = getopt(argc, argv, "hvC:c:L:m:p:C:s:w:")) != -1)
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

            case 'L':
            {
                if (aeron_parse_size64(optarg, &cl.message_length) < 0)
                {
                    fprintf(stderr, "malformed message length %s: %s\n", optarg, aeron_errmsg());
                    exit(status);
                }
                break;
            }

            case 'm':
            {
                if (aeron_parse_size64(optarg, &cl.messages) < 0)
                {
                    fprintf(stderr, "malformed number of messages %s: %s\n", optarg, aeron_errmsg());
                    exit(status);
                }
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

            case 'w':
            {
                if (aeron_parse_size64(optarg, &cl.warm_up_messages) < 0)
                {
                    fprintf(stderr, "malformed number of warm up messages %s: %s\n", optarg, aeron_errmsg());
                    exit(status);
                }
                break;
            }

            case 'h':
            default:
                fprintf(stderr, "Usage: %s %s", argv[0], usage_str);
                exit(status);
        }
    }

    signal(SIGINT, sigint_handler);

    printf("Publishing Ping at channel %s on Stream ID %" PRId32 "\n", cl.ping_channel, cl.ping_stream_id);
    printf("Subscribing Pong at channel %s on Stream ID %" PRId32 "\n", cl.pong_channel, cl.pong_stream_id);

    if (aeron_context_init(&context) < 0)
    {
        fprintf(stderr, "aeron_context_init: %s\n", aeron_errmsg());
        goto cleanup;
    }

    if (NULL != aeron_dir)
    {
        if (aeron_context_set_dir(context, aeron_dir) < 0)
        {
            fprintf(stderr, "aeron_context_set_dir: %s\n", aeron_errmsg());
            goto cleanup;
        }
    }

    if (aeron_init(&cl.aeron, context) < 0)
    {
        fprintf(stderr, "aeron_init: %s\n", aeron_errmsg());
        goto cleanup;
    }

    while ( coroutine_status( cl.r ) && coroutine_status( cl.c ) )
    {
        coroutine_resume( cl.r );
        coroutine_resume( cl.c );
    }

cleanup:
    aeron_close(cl.aeron);
    aeron_context_close(context);

    return status;
}

extern bool is_running();
