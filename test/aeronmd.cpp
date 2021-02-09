/*
 * Copyright 2014-2021 Real Logic Limited.
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
/*#define _GNU_SOURCE*/
#endif

#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#ifndef _MSC_VER
#include <unistd.h>
#endif

extern "C" {
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#include "aeronmd.h"
#include "concurrent/aeron_atomic.h"
#include "aeron_driver_context.h"
#include "media/aeron_udp_channel_transport.h"
#include "media/aeron_udp_transport_poller.h"
#include "media/aeron_udp_channel_transport_bindings.h"
#include "util/aeron_properties_util.h"
#include "util/aeron_strutil.h"
#include "util/aeron_dlopen.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}

volatile bool running = true;

void sigint_handler(int /*signal*/)
{
    AERON_PUT_ORDERED(running, false);
}

void termination_hook(void * /*state*/)
{
    AERON_PUT_ORDERED(running, false);
}

inline bool is_running()
{
    bool result;
    AERON_GET_VOLATILE(result, running);
    return result;
}

int set_property(void * /*clientd*/, const char *name, const char *value)
{
    return aeron_properties_setenv(name, value);
}

aeron_udp_channel_transport_bindings_t * poll;
int poll_fd, timer_fd, max_events = 0;
struct epoll_event event;

int
myudp_poller_init(
    aeron_udp_transport_poller_t          * poller,
    aeron_driver_context_t                * context,
    aeron_udp_channel_transport_affinity_t  affinity )
{
  int status = poll->poller_init_func( poller, context, affinity );
  printf( "poller init fd %d\n", poller->fd );
  return status;
}

int
myudp_poller_close( aeron_udp_transport_poller_t * poller )
{
  printf( "poller close fd %d\n", poller->fd );
  return poll->poller_close_func( poller );
}

int
myudp_poller_add( aeron_udp_transport_poller_t  * poller,
                  aeron_udp_channel_transport_t * transport )
{
  int status;
  printf( "add fd %d\n", transport->fd );
  status = poll->poller_add_func( poller, transport );
  if ( status == 0 ) {
    memset( &event, 0, sizeof( event ) );
    event.data.fd = transport->fd;
    event.events  = EPOLLIN;
    max_events++;
    if ( epoll_ctl( poll_fd, EPOLL_CTL_ADD, transport->fd, &event ) < 0 )
      perror( "epoll_ctl" );
  }
  return status;
}

int
myudp_poller_remove( aeron_udp_transport_poller_t  * poller,
                     aeron_udp_channel_transport_t * transport )
{
  int status;
  printf( "remove fd %d\n", transport->fd );
  status = poll->poller_remove_func( poller, transport );
  if ( status == 0 ) {
    memset( &event, 0, sizeof( event ) );
    event.data.fd = transport->fd;
    event.events  = EPOLLIN;
    max_events--;
    if ( epoll_ctl( poll_fd, EPOLL_CTL_DEL, transport->fd, &event ) < 0 )
      perror( "epoll_ctl" );
  }
  return status;
}

int
myudp_poller_poll(
    aeron_udp_transport_poller_t              * poller,
    struct mmsghdr                            * msgvec,
    size_t                                      vlen,
    int64_t                                   * bytes_rcved,
    aeron_udp_transport_recv_func_t             recv_func,
    aeron_udp_channel_transport_recvmmsg_func_t recvmmsg_func,
    void                                      * clientd )
{
  return poll->poller_poll_func( poller, msgvec, vlen, bytes_rcved,
                                 recv_func, recvmmsg_func, clientd );
}

aeron_udp_channel_transport_bindings_t myudp =
{ aeron_udp_channel_transport_init,
  aeron_udp_channel_transport_close,
  aeron_udp_channel_transport_recvmmsg,
  aeron_udp_channel_transport_sendmmsg,
  aeron_udp_channel_transport_sendmsg,
  aeron_udp_channel_transport_get_so_rcvbuf,
  aeron_udp_channel_transport_bind_addr_and_port,
  myudp_poller_init,
  myudp_poller_close,
  myudp_poller_add,
  myudp_poller_remove,
  myudp_poller_poll,
  { "myudp", "media", NULL, NULL }
};

int main(int argc, char **argv)
{
    int status = EXIT_FAILURE;
    aeron_driver_context_t *context = NULL;
    aeron_driver_t *driver = NULL;
    struct itimerspec ts;
    uint64_t expire;

    int opt;
    int count = 0;

    while ((opt = getopt(argc, argv, "D:v")) != -1)
    {
        switch (opt)
        {
            case 'D':
            {
                aeron_properties_parser_state_t state;
                aeron_properties_parse_init(&state);
                if (aeron_properties_parse_line(&state, optarg, strlen(optarg), set_property, NULL) < 0)
                {
                    fprintf(stderr, "malformed define: %s\n", optarg);
                    exit(status);
                }
                break;
            }

            case 'v':
            {
                printf("%s <%s> major %d minor %d patch %d\n",
                    argv[0], aeron_version_full(), aeron_version_major(), aeron_version_minor(), aeron_version_patch());
                exit(EXIT_SUCCESS);
            }

            default:
                fprintf(stderr, "Usage: %s [-v][-Dname=value]\n", argv[0]);
                exit(status);
        }
    }

    for (int i = optind; i < argc; i++)
    {
        if (aeron_properties_load(argv[i]) < 0)
        {
            fprintf(stderr, "ERROR: loading properties from %s (%d) %s\n", argv[i], aeron_errcode(), aeron_errmsg());
            exit(status);
        }
    }

    signal(SIGINT, sigint_handler);

    poll = aeron_udp_channel_transport_bindings_load_media( "default" );
    if ( poll == NULL ) {
      fprintf( stderr, "poll is null\n" );
      exit( 1 );
    }
    aeron_properties_setenv( "AERON_THREADING_MODE", "SHARED" );
    aeron_properties_setenv( "AERON_UDP_CHANNEL_TRANSPORT_BINDINGS_MEDIA", "myudp" );
    poll_fd  = epoll_create1( 0 );
    timer_fd = timerfd_create( CLOCK_MONOTONIC, TFD_NONBLOCK );
    ts.it_interval.tv_sec  = 0;
    ts.it_interval.tv_nsec = 500;
    ts.it_value.tv_sec     = 0;
    ts.it_value.tv_nsec    = 0;
    timerfd_settime( timer_fd, 0, &ts, NULL );

    event.data.fd = timer_fd;
    event.events  = EPOLLIN;
    if ( epoll_ctl( poll_fd, EPOLL_CTL_ADD, timer_fd, &event ) < 0 )
    {
        perror( "epoll_ctl" );
        goto cleanup;
    }

    if (aeron_driver_context_init(&context) < 0)
    {
        fprintf(stderr, "ERROR: context init (%d) %s\n", aeron_errcode(), aeron_errmsg());
        goto cleanup;
    }

    if (aeron_driver_context_set_driver_termination_hook(context, termination_hook, NULL) < 0)
    {
        fprintf(stderr, "ERROR: context set termination hook (%d) %s\n", aeron_errcode(), aeron_errmsg());
        goto cleanup;
    }

    if (aeron_driver_init(&driver, context) < 0)
    {
        fprintf(stderr, "ERROR: driver init (%d) %s\n", aeron_errcode(), aeron_errmsg());
        goto cleanup;
    }

    if (aeron_driver_start(driver, true) < 0)
    {
        fprintf(stderr, "ERROR: driver start (%d) %s\n", aeron_errcode(), aeron_errmsg());
        goto cleanup;
    }

    while (is_running())
    {
        /*aeron_driver_main_idle_strategy(driver, aeron_driver_main_do_work(driver));*/
        if ( aeron_driver_main_do_work(driver) == 0 )
        {
            if ( count <= 10 )
            {
                proc_yield();
            }
            else if ( count <= 30 )
            {
                sched_yield();
            }
            else if ( max_events > 0 )
            {
                if ( epoll_wait( poll_fd, &event, 1, 1 ) > 0 )
                    if ( ( event.events & EPOLLIN ) != 0 && event.data.fd == timer_fd )
                        read( timer_fd, &expire, sizeof( expire ) );
            }
            else
            {
                usleep( 1 );
            }
            count++;
        }
        else
        {
            count = 0;
        }
    }

    printf("Shutting down driver...\n");

cleanup:
    if (0 != aeron_driver_close(driver))
    {
        fprintf(stderr, "ERROR: driver close (%d) %s\n", aeron_errcode(), aeron_errmsg());
    }

    if (0 != aeron_driver_context_close(context))
    {
        fprintf(stderr, "ERROR: driver context close (%d) %s\n", aeron_errcode(), aeron_errmsg());
    }

    return status;
}

extern bool is_running();
