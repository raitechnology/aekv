#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <aekv/ev_aeron.h>
#include <aeronc.h>
#include <raikv/ev_publish.h>
#include <raikv/delta_coder.h>

using namespace rai;
using namespace aekv;
using namespace kv;

/* poll interval: 100us, hb ival: 200ms, timeout ival: 5s */
static const uint64_t AERON_POLL_US      = 100,
                      AERON_HEARTBEAT_US = 200 * 1000,
                      AERON_TIMEOUT_NS   = AERON_HEARTBEAT_US * 1000 * 25;
static const uint32_t POLL_EVENT_ID = 0,
                      HB_EVENT_ID   = 1;
static char aeron_dbg_path[ 40 ];

EvAeron::EvAeron( EvPoll &p ) noexcept
    : EvSocket( p, p.register_type( "aeron" ) ),
      KvSendQueue( p.map->hdr.create_stamp, p.ctx_id ),
      context( 0 ), aeron( 0 ), pub( 0 ), sub( 0 ), fragment_asm( 0 )
{
  this->timer_id = (uint64_t) this->sock_type << 56;
  this->cur_mono_ns = kv_current_monotonic_coarse_ns();
  ::snprintf( aeron_dbg_path, sizeof( aeron_dbg_path ),
              "/tmp/aeron_dbg.%u", getpid() );
  printf( "debug: %s\n", aeron_dbg_path );
  p.add_route_notify( *this );
}

void
print_avail_img( void *, aeron_subscription_t *subscription,
                 aeron_image_t *image )
{
  aeron_subscription_constants_t subscription_constants;
  aeron_image_constants_t        image_constants;

  if ( aeron_subscription_constants( subscription, &subscription_constants ) <
         0 ||
       aeron_image_constants( image, &image_constants ) < 0 ) {
    fprintf( stderr, "could not get subscription/image constants: %s\n",
             aeron_errmsg() );
  }
  else {
    printf( "Available image on %s streamId=%d sessionId=%d from %s\n",
            subscription_constants.channel, subscription_constants.stream_id,
            image_constants.session_id, image_constants.source_identity );
  }
}

void
print_unavail_img( void *, aeron_subscription_t *subscription,
                   aeron_image_t *image )
{
  aeron_subscription_constants_t subscription_constants;
  aeron_image_constants_t        image_constants;

  if ( aeron_subscription_constants( subscription, &subscription_constants ) <
         0 ||
       aeron_image_constants( image, &image_constants ) < 0 ) {
    fprintf( stderr, "could not get subscription/image constants: %s\n",
             aeron_errmsg() );
  }
  else {
    printf( "Unavailable image on %s streamId=%d sessionId=%d\n",
            subscription_constants.channel, subscription_constants.stream_id,
            image_constants.session_id );
  }
}
/* allocate aeron client */
EvAeron *
EvAeron::create_aeron( EvPoll &p,  const char *pub_channel,
                       int pub_stream_id,  const char *sub_channel,
                       int sub_stream_id ) noexcept
{
  void * m = aligned_malloc( sizeof( EvAeron ) );
  if ( m == NULL ) {
    perror( "alloc aeron" );
    return NULL;
  }
  EvAeron * a = new ( m ) EvAeron( p );
  int pfd = p.get_null_fd();
  a->PeerData::init_peer( pfd, NULL, "aeron" );
  a->sock_opts = kv::OPT_NO_POLL | kv::OPT_NO_CLOSE;
  if ( p.add_sock( a ) < 0 ) {
    fprintf( stderr, "failed to add aeron\n" );
    return NULL;
  }
  if ( ! a->init_aeron( pub_channel, pub_stream_id, sub_channel,
                        sub_stream_id ) ) {
    fprintf( stderr, "failed to init aeron: %s\n", aeron_errmsg() );
    a->release();
    p.remove_sock( a );
    return NULL;
  }
  p.add_timer_micros( pfd, AERON_POLL_US, a->timer_id, POLL_EVENT_ID );
  p.add_timer_micros( pfd, AERON_HEARTBEAT_US, a->timer_id, HB_EVENT_ID );
  a->create_kvmsg( KV_MSG_HELLO, sizeof( KvMsg ) );
  a->idle_push( EV_WRITE );
  return a;
}
/* tell the aeron driver which sub and pub streams are used */
bool
EvAeron::init_aeron( const char *pub_channel,  int pub_stream_id,
                     const char *sub_channel,  int sub_stream_id ) noexcept
{
  aeron_async_add_publication_t  * async_pub = NULL;
  aeron_async_add_subscription_t * async_sub = NULL;
  int status = aeron_context_init( &this->context );
  if ( status == 0 )
    status = aeron_init( &this->aeron, this->context );
  if ( status == 0 )
    status = aeron_start( this->aeron );
  if ( status == 0 )
    status = aeron_async_add_publication( &async_pub, this->aeron, pub_channel,
                                          pub_stream_id );
  if ( status == 0 ) {
    for (;;) {
      status = aeron_async_add_publication_poll( &this->pub, async_pub );
      if ( status != 0 ) /* -1 = error, 1 = success */
        break;
    }
  }
  if ( status >= 0 )
    status = aeron_async_add_subscription( &async_sub, this->aeron, sub_channel,
                                           sub_stream_id, print_avail_img,
                                           NULL, print_unavail_img, NULL );
  if ( status == 0 ) {
    for (;;) {
      status = aeron_async_add_subscription_poll( &this->sub, async_sub );
      if ( status != 0 )
        break;
    }
  }
  if ( status >= 0 )
    status = aeron_fragment_assembler_create( &this->fragment_asm,
                                              EvAeron::poll_handler, this );
  return status == 0;
}
/* send the messages queued */
void
EvAeron::write( void ) noexcept
{
  while ( ! this->sendq.is_empty() ) {
    KvMsgList * l = this->sendq.hd;
    if ( aeron_publication_offer( this->pub, (const uint8_t *) (void *) &l->msg,
                                  l->msg.size, NULL, NULL ) <= 0 ) {
      return;
    }
    /*printf( "### write:" );
    l->msg.print();*/
    this->sendq.pop_hd();
  }
  this->snd_wrk.reset();
  this->pop( EV_WRITE );
}
/* read is handled by poll(), which is a timer based event */
void EvAeron::read( void ) noexcept {}
void EvAeron::process( void ) noexcept {}
/* shutdown aeron client */
void
EvAeron::process_shutdown( void ) noexcept
{
  this->poll.remove_route_notify( *this );
}
/* close the aeron sub/pub streams */
void
EvAeron::release( void ) noexcept
{
  if ( this->sub != NULL )
    aeron_subscription_close( this->sub, NULL, NULL );
  if ( this->pub != NULL )
    aeron_publication_close( this->pub, NULL, NULL );
  if ( this->aeron != NULL )
    aeron_close( this->aeron );
  if ( this->context != NULL )
    aeron_context_close( this->context );
  if ( this->fragment_asm != NULL )
    aeron_fragment_assembler_delete( this->fragment_asm );

  this->sub          = NULL;
  this->pub          = NULL;
  this->aeron        = NULL;
  this->context      = NULL;
  this->fragment_asm = NULL;
}
/* timer based events, poll for new messages, send heartbeats,
 * check for session timeouts  */
bool
EvAeron::timer_expire( uint64_t,  uint64_t event_id ) noexcept
{
  this->cur_mono_ns = kv_current_monotonic_coarse_ns();
  switch ( event_id ) {
    case POLL_EVENT_ID: {
      static const uint32_t fragment_count_limit = 10;
      int fragments_read;
      
      for (;;) {
        fragments_read = aeron_subscription_poll( this->sub,
                                               aeron_fragment_assembler_handler,
                                                  this->fragment_asm,
                                                  fragment_count_limit );
        if ( fragments_read <= 0 ) {
          if ( fragments_read == 0 )
            break;
          fprintf( stderr, "aeron_subscription_poll: %s\n", aeron_errmsg() );
          this->push( EV_CLOSE );
          break;
        }
      }
      break;
    }
    case HB_EVENT_ID: {
      if ( ::unlink( aeron_dbg_path ) == 0 )
        this->print_stats();
      AeronSession *session =
        this->my_peers.check_timeout( this->cur_mono_ns - AERON_TIMEOUT_NS );
      if ( session != NULL ) {
        this->send_dataloss( *session );
        this->my_peers.release_session( *session );
      }
      this->create_kvmsg( KV_MSG_HELLO, sizeof( KvMsg ) );
      this->idle_push( EV_WRITE );
      break;
    }
  }
  return true;
}
/* when new subscription occurs by an in process bridge */
void
EvAeron::on_sub( uint32_t h,  const char *sub,  size_t sublen,
                 uint32_t src_fd,  uint32_t /*rcnt*/,  char src_type,
                 const char *rep,  size_t rlen ) noexcept
{
  if ( src_fd == (uint32_t) this->fd )
    return;
  KvSubMsg *submsg =
    this->create_kvsubmsg( h, sub, sublen, src_type, KV_MSG_SUB, 'L', rep,
                           rlen );
  this->my_subs.upsert( *submsg );
}
/* when an unsubscribe occurs by an in process bridge */
void
EvAeron::on_unsub( uint32_t h,  const char *sub,  size_t sublen,
                   uint32_t src_fd,  uint32_t rcnt,  char src_type ) noexcept
{
  if ( src_fd == (uint32_t) this->fd )
    return;
  bool do_unsubscribe = false;
  if ( rcnt == 0 ) /* no more routes left */
    do_unsubscribe = true;
  else if ( rcnt == 1 ) { /* if the only route left is not in my server */
    if ( this->poll.sub_route.is_sub_member( h, this->fd ) )
      do_unsubscribe = true;
  }
  KvSubMsg *submsg =
    this->create_kvsubmsg( h, sub, sublen, src_type, KV_MSG_UNSUB,
                           do_unsubscribe ? 'D' : 'C', NULL, 0 );
  if ( do_unsubscribe )
    this->my_subs.remove( *submsg );
}
/* a new pattern subscription by an in process bridge */
void
EvAeron::on_psub( uint32_t h,  const char *pattern,  size_t patlen,
                  const char *prefix,  uint8_t prefix_len,
                  uint32_t src_fd,  uint32_t /*rcnt*/,  char src_type ) noexcept
{
  if ( src_fd == (uint32_t) this->fd )
    return;
  KvSubMsg *submsg =
    this->create_kvpsubmsg( h, pattern, patlen, prefix, prefix_len, src_type,
                            KV_MSG_PSUB, 'L' );
  this->my_subs.upsert( *submsg );
}
/* a new pattern unsubscribe by an in process bridge */
void
EvAeron::on_punsub( uint32_t h,  const char *pattern,  size_t patlen,
                    const char *prefix,  uint8_t prefix_len,
                    uint32_t src_fd,  uint32_t rcnt,  char src_type ) noexcept
{
  if ( src_fd == (uint32_t) this->fd )
    return;
  bool do_unsubscribe = false;
  if ( rcnt == 0 ) /* no more routes left */
    do_unsubscribe = true;
  else if ( rcnt == 1 ) { /* if the only route left is not in my server */
    if ( this->poll.sub_route.is_sub_member( h, this->fd ) )
      do_unsubscribe = true;
  }
  KvSubMsg *submsg =
    this->create_kvpsubmsg( h, pattern, patlen, prefix, prefix_len, src_type,
                            KV_MSG_PUNSUB, do_unsubscribe ? 'D' : 'C' );
  if ( do_unsubscribe )
    this->my_subs.remove_pattern( *submsg );
}
/* when new client appers on the network, publish my subscriptions */
void
EvAeron::publish_my_subs( void ) noexcept
{
  uint32_t i = 0, j = 0;
  while ( i < this->my_subs.subs_cnt ) {
    KvSubMsg &scan = *(KvSubMsg *) (void *) &this->my_subs.subs[ i + 1 ];
    if ( scan.sublen != 0 ) {
      KvSubMsg & msg = *this->KvSendQueue::copy_kvsubmsg( scan );
      msg.set_seqno( ++this->KvSendQueue::next_seqno );
      printf( "puslish_sub: %.*s\n", msg.sublen, msg.subject() );
    }
    i += MySubs::subs_align( scan.size ) / sizeof( uint32_t ) + 1;
    j++;
  }
  printf( "pub %u cnt %u\n", i, j );
  this->idle_push( EV_WRITE );
}
/* a cache for subscritions */
MySubs::MySubs() noexcept
{
  this->subsc_idx = UIntHashTab::resize( NULL );
  this->subs      = NULL;
  this->subs_free = 0;
  this->subs_cnt  = 0;
  this->subs_size = 0;
}
/* append submsg to cache */
uint32_t
MySubs::append( KvSubMsg &msg ) noexcept
{
  uint32_t i = subs_align( msg.size ) / sizeof( uint32_t ) + 1,
           j = this->subs_cnt + i;
  if ( j > this->subs_size ) {
    uint32_t sz = ( ( j + 1 ) | 255 ) + 1;
    void   * p  = ::realloc( this->subs, sz * sizeof( uint32_t ) );
    if ( p == NULL ) {
      perror( "realloc subs" );
      return 0;
    }
    this->subs = (uint32_t *) p;
    this->subs_size = sz;
  }
  this->subs[ this->subs_cnt ] = 0;
  ::memcpy( &this->subs[ this->subs_cnt + 1 ], &msg, msg.size );
  this->subs_cnt += i;
  return this->subs_cnt - i + 1;
}
/* insert submsg, check for duplicates and maintain a list for hash collisions*/
void
MySubs::upsert( KvSubMsg &msg ) noexcept
{
  uint32_t pos, head, next, prev, i;

  if ( this->subs_free * 2 > this->subs_size && this->subs_free > 1024 )
    this->gc();
  if ( this->subsc_idx->find( msg.hash, pos, head ) ) {
    prev = 0;
    for ( i = head; i != 0; i = this->subs[ i - 1 ] ) {
      KvSubMsg & htmsg = *(KvSubMsg *) (void *) &this->subs[ i ];
      /* update the subscription message */
      if ( msg.hash == htmsg.hash && msg.subject_equals( htmsg ) ) {
        /* replace existing by copying over it */
        if ( msg.size == htmsg.size ) {
          ::memcpy( &htmsg, &msg, msg.size );
          return;
        }
        /* old message is different size, append and update hash */
        this->subs_free += subs_align( htmsg.size ) / sizeof( uint32_t ) + 1;
        htmsg.sublen = 0; /* remove existing */
        next = this->append( msg );
        if ( i == head ) { /* replace at head */
          this->subs[ next - 1 ] = this->subs[ i - 1 ];
          this->subsc_idx->set( msg.hash, pos, next );
        }
        else { /* replace in chain [ prev ] -> [ next ] -> [ i ] */
          this->subs[ prev - 1 ] = next;
          this->subs[ next - 1 ] = this->subs[ i - 1 ];
        }
        return;
      }
      prev = i;
    }
    /* append to tail of chain */
    next = this->append( msg );
    this->subs[ prev - 1 ] = next;
    return;
  }
  /* not found, insert */
  head = this->append( msg );
  this->subsc_idx->set( msg.hash, pos, head );
  if ( this->subsc_idx->need_resize() )
    this->subsc_idx = UIntHashTab::resize( this->subsc_idx );
}
/* remove subscriton from cache */
void
MySubs::remove( KvSubMsg &msg ) noexcept
{
  uint32_t pos, head, prev, i;

  if ( this->subsc_idx->find( msg.hash, pos, head ) ) {
    prev = 0;
    for ( i = head; i != 0; i = this->subs[ i - 1 ] ) {
      KvSubMsg & htmsg = *(KvSubMsg *) (void *) &this->subs[ i ];
      /* update the subscription message */
      if ( msg.subject_equals( htmsg ) ) {
        /* free it */
        this->subs_free += subs_align( htmsg.size ) / sizeof( uint32_t ) + 1;
        htmsg.sublen = 0; /* remove existing */
        if ( i == head ) {
          /* remove from head */
          if ( this->subs[ i - 1 ] == 0 ) {
            this->subsc_idx->remove( pos ); /* no more subs */
            if ( this->subsc_idx->need_resize() )
              this->subsc_idx = UIntHashTab::resize( this->subsc_idx );
          }
          else
            this->subsc_idx->set( msg.hash, pos, this->subs[ i - 1 ] );
        }
        else {
          /* remove from chain */
          this->subs[ prev - 1 ] = this->subs[ i - 1 ];
        }
        return;
      }
      prev = i;
    }
  }
  printf( "unsub: %.*s not found\n", msg.sublen, msg.subject() );
}
/* remove a pattern subscription from cache, patterns are hashed by prefix */
void
MySubs::remove_pattern( KvSubMsg &msg ) noexcept
{
  uint32_t pos, head, prev, i;

  if ( this->subsc_idx->find( msg.hash, pos, head ) ) {
    prev = 0;
    for ( i = head; i != 0; i = this->subs[ i - 1 ] ) {
      KvSubMsg & htmsg = *(KvSubMsg *) (void *) &this->subs[ i ];
      /* update the subscription message */
      if ( msg.reply_equals( htmsg ) ) {
        /* free it */
        this->subs_free += subs_align( htmsg.size ) / sizeof( uint32_t ) + 1;
        htmsg.sublen = 0; /* remove existing */
        if ( i == head ) {
          /* remove from head */
          if ( this->subs[ i - 1 ] == 0 ) {
            this->subsc_idx->remove( pos ); /* no more subs */
            if ( this->subsc_idx->need_resize() )
              this->subsc_idx = UIntHashTab::resize( this->subsc_idx );
            return;
          }
          else {
            head = this->subs[ i - 1 ];
            this->subsc_idx->set( msg.hash, pos, head );
          }
        }
        else {
          /* remove from chain */
          this->subs[ prev - 1 ] = this->subs[ i - 1 ];
        }
      }
      else {
        prev = i;
      }
    }
  }
}
/* recover space by moving active elements to head of subs[] array */
void
MySubs::gc( void ) noexcept
{
  uint32_t i = 0, j = 0;

  this->subsc_idx->clear_all();
  while ( i < this->subs_cnt ) {
    KvSubMsg &scan = *(KvSubMsg *) (void *) &this->subs[ i + 1 ];
    uint32_t k = subs_align( scan.size );
    if ( scan.sublen != 0 ) {
      if ( i != j )
        ::memmove( &this->subs[ j + 1 ], &this->subs[ i + 1 ], k );
      KvSubMsg & msg = *(KvSubMsg *) (void *) &this->subs[ j + 1 ];
      uint32_t pos, next;
      /* if collision */
      if ( this->subsc_idx->find( msg.hash, pos, next ) )
        this->subs[ j ] = next;
      else
        this->subs[ j ] = 0;
      this->subsc_idx->set( msg.hash, pos, j + 1 );
      j += k / sizeof( uint32_t ) + 1;
    }
    i += k / sizeof( uint32_t ) + 1;
  }
  this->subs_cnt  = j;
  this->subs_free = 0;
}
/* publish a message from bridge proto to aeron network */
bool
EvAeron::on_msg( EvPublish &pub ) noexcept
{
  /* no publish to self */
  if ( (uint32_t) this->fd != pub.src_route ) {
    this->create_kvpublish( pub.subj_hash, pub.subject, pub.subject_len,
                            pub.prefix, pub.hash, pub.prefix_cnt,
                            (const char *) pub.reply, pub.reply_len, pub.msg,
                            pub.msg_len, pub.pub_type, pub.msg_enc );
    this->idle_push( EV_WRITE );
  }
/*  if ( this->backlogq.is_empty() )*/
    return true;
  /* hash backperssure, could be more specific for the stream destination */
/*  return false; */
}
/* recv a message from aeron network and route to bridge protos */
void
EvAeron::on_poll_handler( const uint8_t *buffer,  size_t length,
                          aeron_header_t * ) noexcept
{
  KvMsg  & msg = *(KvMsg *) (void *) buffer;

  if ( ! msg.is_valid( length ) ) {
    fprintf( stderr, "Invalid message, length %lu\n", length );
    KvHexDump::dump_hex( buffer, length < 256 ? length : 256 );
    return;
  }
  else if ( msg.get_stamp() == this->stamp ) {
    if ( msg.src == this->send_src )
      return;
    fprintf( stderr, "Loop with source %u\n", msg.src );
    msg.print();
    return;
  }
  /*printf( "### on_poll_handler:" );
  msg.print();*/
  /*msg.print();*/
  /*uint64_t seqno = msg.get_seqno();*/
/*  if ( seqno != this->last_seqno[ msg.src ] + 1 ) {
    printf( "missing seqno %lu -> %lu\n", this->last_seqno[ msg.src ],
            seqno );
  }
  this->last_seqno[ msg.src ] = seqno;*/
  AeronSession * session = this->my_peers.update_session( msg.get_stamp(),
                                                          msg.get_seqno() );
  if ( session == NULL )
    return;
  if ( session->test( SESSION_DATALOSS ) ) {
    session->clear( SESSION_DATALOSS );
    if ( msg.msg_type != KV_MSG_BYE )
      this->send_dataloss( *session );
  }
  session->last_active = this->cur_mono_ns;

  if ( msg.msg_type == KV_MSG_PUBLISH ) {
    /* forward message from publisher to shm */
    session->pub_count++;
    KvSubMsg & submsg = (KvSubMsg &) msg;
    EvPublish pub( submsg.subject(), submsg.sublen,
                   submsg.reply(), submsg.replylen,
                   submsg.get_msg_data(), submsg.msg_size,
                   this->fd, submsg.hash, NULL, 0,
                   submsg.msg_enc, submsg.code );
    this->poll.forward_msg( pub, NULL, submsg.get_prefix_cnt(),
                            submsg.prefix_array() );
    return;
  }

  if ( session->test( SESSION_NEW ) ) {
    session->clear( SESSION_NEW );
    if ( msg.msg_type != KV_MSG_BYE )
      this->publish_my_subs();
  }

  AeronSubStatus stat;
  int            rcnt;
  switch ( msg.msg_type ) {
    case KV_MSG_FRAGMENT:
      break;
    case KV_MSG_SUB: { /* update my routing table when sub/unsub occurs */
      KvSubMsg &submsg = (KvSubMsg &) msg;
      rcnt = 2; /* if alredy exists, there are at least 2 */
      stat = this->sub_tab.put( submsg.hash, submsg.subject(),
                                submsg.sublen, session->id );
      if ( stat == AERON_SUB_NEW ) {
        printf( "new_sub: %.*s\n", submsg.sublen, submsg.subject() );
        rcnt = this->poll.sub_route.add_sub_route( submsg.hash, this->fd );
        session->sub_count++; /* session was added */
      }
      /*if ( stat != AERON_SUB_EXISTS )*/
      this->poll.notify_sub( submsg.hash, submsg.subject(), submsg.sublen,
                             this->fd, rcnt, 'A',
                             submsg.reply(), submsg.replylen );
      break;
    }
    case KV_MSG_UNSUB: {
      KvSubMsg &submsg = (KvSubMsg &) msg;
      rcnt = 2;
      if ( submsg.code == 'D' ) { /* subscription is retired, remove route */
        stat = this->sub_tab.rem( submsg.hash, submsg.subject(), submsg.sublen,
                                  session->id );
        if ( stat == AERON_SUB_REMOVED ) {
          printf( "rem_sub: %.*s\n", submsg.sublen, submsg.subject() );
          if ( this->sub_tab.tab.find_by_hash( submsg.hash ) == NULL )
            rcnt = this->poll.sub_route.del_sub_route( submsg.hash, this->fd );
          session->sub_count--;
        }
      }
      /*if ( stat != AERON_SUB_NOT_FOUND )*/
      this->poll.notify_unsub( submsg.hash, submsg.subject(), submsg.sublen,
                               this->fd, rcnt, 'A' );
      break;
    }
    case KV_MSG_PSUB: {
      KvSubMsg &submsg = (KvSubMsg &) msg;
      rcnt = 2;
      stat = this->pat_sub_tab.put( submsg.hash, submsg.subject(),
                                    submsg.sublen + submsg.replylen + 2,
                                    submsg.replylen, session->id );
      if ( stat == AERON_SUB_NEW ) {
        printf( "add_psub: %.*s\n", submsg.sublen, submsg.subject() );
        rcnt = this->poll.sub_route.add_pattern_route( submsg.hash, this->fd,
                                                       submsg.replylen );
        session->psub_count++; /* session was added */
      }
      this->poll.notify_psub( submsg.hash, submsg.subject(), submsg.sublen,
                              submsg.reply(), submsg.replylen,
                              this->fd, rcnt, 'A' );
      break;
    }
    case KV_MSG_PUNSUB: {
      KvSubMsg &submsg = (KvSubMsg &) msg;
      AeronTmpList tmp;
      rcnt = 2;
      if ( submsg.code == 'D' ) { /* subscription is retired, remove route */
        stat = this->pat_sub_tab.rem( submsg.hash, submsg.reply(),
                                      submsg.replylen, session->id, tmp ); 
        if ( stat == AERON_SUB_OK ) {
          if ( tmp.list.hd != NULL ) {
            for ( AeronTmpElem *el = tmp.list.hd; el != NULL; el = el->next )
              printf( "rem_psub: %.*s\n", (int) el->x.pattern_len(),
                                                el->x.pattern() );
            rcnt =
              this->poll.sub_route.del_pattern_route( submsg.hash, this->fd,
                                                      submsg.replylen );
          }
          session->psub_count--;
        }
        else {
          printf( "stat %d\n", stat );
        }
      }
      else {
        printf( "code %c\n", submsg.code );
      }
      for ( AeronTmpElem *el = tmp.list.hd; el != NULL; el = el->next ) {
        this->poll.notify_punsub( submsg.hash, el->x.pattern(),
                                  el->x.pattern_len(), el->x.prefix(),
                                  el->x.prefix_len(), this->fd, rcnt, 'A' );
      }
      break;
    }
    case KV_MSG_HELLO:
      break;
    case KV_MSG_BYE:
      session->clear();
      session->set( SESSION_BYE );
      this->clear_session( *session );
      this->my_peers.release_session( *session );
      break;
    default:
      break;
  }
}
/* aeron callback to recv a message */
void
EvAeron::poll_handler( void *clientd, const uint8_t *buffer,
                       size_t length,  aeron_header_t *header )
{
  ((EvAeron *) clientd)->on_poll_handler( buffer, length, header );
}
/* if a publisher from the aeron network loses sequences or times out */
void
EvAeron::send_dataloss( AeronSession &session ) noexcept
{
  if ( session.test( SESSION_DATALOSS ) )
    printf( "session %u stamp %lu missing seqno=%lu\n", session.id,
            session.stamp, session.delta_seqno );
  if ( session.test( SESSION_TIMEOUT ) )
    printf( "session %u stamp %lu timeout\n", session.id, session.stamp );
  this->clear_session( session );
}
/* clear session of subscriptions and patterns open */
void
EvAeron::clear_session( AeronSession &session ) noexcept
{
  if ( session.sub_count != 0 )
    this->clear_subs( session );
  if ( session.psub_count != 0 )
    this->clear_pattern_subs( session );
  /* clear state bits and set to NEW */
  session.clear();
  session.set( SESSION_NEW );
}
/* remove all sub routes */
void
EvAeron::clear_all_subs( void ) noexcept
{
  AeronSubRoutePos pos;
  AeronPatternSubRoutePos ppos;
  uint32_t rcnt;
  if ( this->sub_tab.first( pos ) ) {
    do {
      rcnt = this->poll.sub_route.del_sub_route( pos.rt->hash, this->fd );
      this->poll.notify_unsub( pos.rt->hash, pos.rt->value, pos.rt->len,
                               this->fd, rcnt, 'A' );
    } while ( this->sub_tab.next( pos ) );
  }
  if ( this->pat_sub_tab.first( ppos ) ) {
    do {
      rcnt = this->poll.sub_route.del_pattern_route( ppos.rt->hash, this->fd,
                                                     ppos.rt->prefix_len() );
      this->poll.notify_punsub( ppos.rt->hash, ppos.rt->pattern(),
                                ppos.rt->pattern_len(), ppos.rt->prefix(),
                                ppos.rt->prefix_len(), this->fd, rcnt, 'A' );
    } while ( this->pat_sub_tab.next( ppos ) );
  }
}
/* clear session from sub routes, notify bridges of unsubscribe */
void
EvAeron::clear_subs( AeronSession &session ) noexcept
{
  AeronTmpList tmp;
  AeronSubRoutePos pos;
  AeronSubStatus stat;
  uint32_t id = session.id;

  if ( this->sub_tab.first( pos ) ) {
    do {
      uint32_t rcnt = 2;
      stat = AeronSubMap::remove_sub( this->sub_tab.zip, pos.rt->sub, id );
      if ( stat == AERON_SUB_REMOVED ) {
        if ( this->sub_tab.tab.find_by_hash( pos.rt->hash ) == NULL ) {
          rcnt = this->poll.sub_route.del_sub_route( pos.rt->hash, this->fd );
          tmp.append( *pos.rt );
        }
      }
      if ( stat != AERON_SUB_NOT_FOUND ) {
        this->poll.notify_unsub( pos.rt->hash, pos.rt->value, pos.rt->len,
                                 this->fd, rcnt, 'A' );
      }
    } while ( this->sub_tab.next( pos ) );
  }
  for ( AeronTmpElem *el = tmp.list.hd; el != NULL; el = el->next )
    this->sub_tab.tab.remove( el->x.hash, el->x.value, el->x.len );
  session.sub_count = 0;
}
/* clear session from pattern routes, notify bridges of punsubscribe */
void
EvAeron::clear_pattern_subs( AeronSession &session ) noexcept
{
  AeronTmpList tmp;
  AeronPatternSubRoutePos pos;
  AeronSubStatus stat;
  uint32_t id = session.id;

  if ( this->pat_sub_tab.first( pos ) ) {
    do {
      uint32_t rcnt = 2;
      stat = AeronSubMap::remove_sub( this->pat_sub_tab.zip, pos.rt->sub, id );
      if ( stat == AERON_SUB_REMOVED ) {
        rcnt = this->poll.sub_route.del_pattern_route( pos.rt->hash, this->fd,
                                                       pos.rt->prefix_len() );
        tmp.append( *pos.rt );
      }
      if ( stat != AERON_SUB_NOT_FOUND )
        this->poll.notify_punsub( pos.rt->hash, pos.rt->pattern(),
                                  pos.rt->pattern_len(), pos.rt->prefix(),
                                  pos.rt->prefix_len(), this->fd, rcnt, 'A' );
    } while ( this->pat_sub_tab.next( pos ) );
  }
  for ( AeronTmpElem *el = tmp.list.hd; el != NULL; el = el->next )
    this->pat_sub_tab.tab.remove( el->x.hash, el->x.value, el->x.len );
  session.psub_count = 0;
}
/* list of all sessions on aeron network */
MyPeers::MyPeers() noexcept
       : dummy_session( 0 )
{
  this->session_idx   = UIntHashTab::resize( NULL );
  this->last_session  = &this->dummy_session;
  this->sessions      = NULL;
  this->session_size  = 0;
  this->last_check_ns = 0;
}
/* creae a new session and index by stamp */
AeronSession *
MyPeers::new_session( uint64_t stamp,  uint64_t seqno, uint32_t h,
                      uint32_t pos,  AeronSession *next_id ) noexcept
{
  if ( this->free_list.is_empty() ) {
    void *p = ::realloc( this->sessions,
                  sizeof( this->sessions[ 0 ] ) * ( this->session_size + 64 ) );
    if ( p == NULL ) {
      perror( "realloc net_session" );
      return NULL;
    }
    this->sessions = (AeronSession **) p;
    p = ::malloc( sizeof( AeronSession ) * 64 );
    if ( p == NULL ) {
      perror( "alloc sessions" );
      return NULL;
    }
    for ( int i = 0; i < 64; i++ ) {
      this->sessions[ this->session_size ] = NULL;
      AeronSession * x = new ( p ) AeronSession( this->session_size++ );
      this->free_list.push_tl( x );
      p = (void *) &x[ 1 ];
    }
  }

  this->last_session = this->free_list.pop_hd();
  uint32_t id = this->last_session->id;
  this->session_idx->set( h, pos, id );
  if ( this->session_idx->need_resize() )
    this->session_idx = UIntHashTab::resize( this->session_idx );

  printf( "new session %u seqno=%lu stamp=%lu\n", id, seqno, stamp );
  this->sessions[ id ] = this->last_session;
  new ( this->last_session ) AeronSession( id, stamp, seqno, next_id );
  this->list.push_hd( this->last_session );
  return this->last_session;
}
/* release a session by removing from index, put to free list for reuse */
void
MyPeers::release_session( AeronSession &session ) noexcept
{
  uint32_t h = hash( session.stamp ), pos, id;
  if ( &session == this->last_session )
    this->last_session = &this->dummy_session;
  if ( this->session_idx->find( h, pos, id ) ) {
    /* if head of chain */
    if ( id == session.id ) {
      /* if more sessions follow */
      if ( session.next_id != NULL ) {
        this->session_idx->set( h, pos, session.next_id->id );
        session.next_id->last_id = NULL;
        session.next_id = NULL;
      }
      /* is only link  in chain */
      else {
        this->session_idx->remove( pos );
        if ( this->session_idx->need_resize() )
          this->session_idx = UIntHashTab::resize( this->session_idx );
      }
    }
    /* find link in chain */
    else {
      for ( AeronSession *p = this->sessions[ id ]->next_id; ; ) {
        if ( p == &session ) {
          p->last_id->next_id = p->next_id;
          if ( p->next_id != NULL )
            p->next_id->last_id = p->last_id;
          p->next_id = p->last_id = NULL;
          break;
        }
        p = p->next_id;
      }
    }
    this->list.pop( &session );
    this->free_list.push_tl( &session );
    printf( "release session %u stamp=%lu\n", session.id, session.stamp );
  }
  else {
    fprintf( stderr, "session %u stamp=%lu not found!\n",
             session.id, session.stamp );
  }
}
/* merge id into tab[ subj ] route */
AeronSubStatus
AeronSubMap::merge_sub( RouteZip &zip,  uint32_t &r,  uint32_t i ) noexcept
{
  uint32_t * routes;
  CodeRef  * p = NULL;
  uint32_t   rcnt = zip.decompress_routes( r, routes, p ),
             xcnt = RouteZip::insert_route( i, routes, rcnt );
  if ( xcnt != rcnt ) {
    r = zip.compress_routes( routes, xcnt );
    zip.deref_codep( p );
    return AERON_SUB_OK;
  }
  return AERON_SUB_EXISTS;
}
/* remove id from tab[ subj ] route */
AeronSubStatus
AeronSubMap::remove_sub( RouteZip &zip,  uint32_t &r,  uint32_t i ) noexcept
{
  uint32_t * routes;
  CodeRef  * p = NULL;
  uint32_t   rcnt = zip.decompress_routes( r, routes, p ),
             xcnt = RouteZip::delete_route( i, routes, rcnt );
  if ( xcnt != rcnt ) {
    if ( xcnt > 0 )
      r = zip.compress_routes( routes, xcnt );
    else
      r = 0;
    zip.deref_codep( p );
    if ( xcnt == 0 )
      return AERON_SUB_REMOVED;
    return AERON_SUB_OK;
  }
  return AERON_SUB_NOT_FOUND;
}
/* new id in tab[ sub ] route */
uint32_t
AeronSubMap::make_sub( uint32_t i ) noexcept
{
  return DeltaCoder::encode( 1, &i, 0 );
}

void
AeronSubMap::print( void ) noexcept
{
  AeronSubRoutePos pos;
  if ( this->first( pos ) ) {
    do {
      uint32_t * routes;
      CodeRef  * p    = NULL;
      uint32_t   rcnt = this->zip.decompress_routes( pos.rt->sub, routes, p );

      printf( "%.*s: [ %u", (int) pos.rt->len, pos.rt->value, routes[ 0 ] );
      for ( uint32_t i = 1; i < rcnt; i++ )
        printf( ", %u", routes[ i ] );
      printf( " ] (session-ids)\n" );
    } while ( this->next( pos ) );
  }
}

void
AeronPatternSubMap::print( void ) noexcept
{
  AeronPatternSubRoutePos pos;
  if ( this->first( pos ) ) {
    do {
      uint32_t * routes;
      CodeRef  * p    = NULL;
      uint32_t   rcnt = this->zip.decompress_routes( pos.rt->sub, routes, p );

      printf( "%.*s: [ %u", (int) pos.rt->pattern_len(), pos.rt->pattern(),
               routes[ 0 ] );
      for ( uint32_t i = 1; i < rcnt; i++ )
        printf( ", %u", routes[ i ] );
      printf( " ] (session-ids)\n" );
    } while ( this->next( pos ) );
  }
}

void
MyPeers::print( void ) noexcept
{
  for ( AeronSession *s = this->list.hd; s != NULL; s = s->next ) {
    printf( "session-id %u = %lu.%lu subs=%u psubs=%u pubs=%lu\n",
            s->id, s->stamp, s->last_seqno, s->sub_count, s->psub_count,
            s->pub_count );
  }
}

void
MySubs::print( EvPoll &poll ) noexcept
{
  uint32_t i = 0;
  while ( i < this->subs_cnt ) {
    KvSubMsg &scan = *(KvSubMsg *) (void *) &this->subs[ i + 1 ];
    uint32_t k = subs_align( scan.size );
    if ( scan.sublen != 0 ) {
      if ( scan.msg_type == KV_MSG_PSUB ) {
        printf( "%.*s (%.*s) rcnt %u\n",
                (int) scan.sublen, scan.subject(),
                (int) scan.replylen, scan.reply(),
                poll.sub_route.get_route_count( scan.replylen, scan.hash ) );
      }
      else {
        printf( "%.*s rcnt %u\n", (int) scan.sublen, scan.subject(),
                poll.sub_route.get_sub_route_count( scan.hash ) );
      }
    }
    i += k / sizeof( uint32_t ) + 1;
  }
}

void
EvAeron::print_stats( void ) noexcept
{
  printf( "+-------------------+\n" );
  printf( "|- AeronSubMap -----|\n" );
  this->sub_tab.print();
  printf( "|- AeronPatternMap -|\n" );
  this->pat_sub_tab.print();
  printf( "|- MySubs ----------|\n" );
  this->my_subs.print( this->poll );
  printf( "|- MyPeers ---------|\n" );
  this->my_peers.print();
  printf( "+-------------------+\n" );
  fflush( stdout );
}
