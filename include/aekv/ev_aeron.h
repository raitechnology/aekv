#ifndef __rai_aekv__ev_aeron_h__
#define __rai_aekv__ev_aeron_h__

extern "C" {
typedef struct aeron_stct                    aeron_t;
typedef struct aeron_context_stct            aeron_context_t;
typedef struct aeron_publication_stct        aeron_publication_t;
typedef struct aeron_subscription_stct       aeron_subscription_t;
typedef struct aeron_fragment_assembler_stct aeron_fragment_assembler_t;
typedef struct aeron_header_stct             aeron_header_t;
}

#include <raikv/ev_net.h>
#include <raikv/kv_msg.h>
#include <raikv/kv_pubsub.h>
#include <raikv/route_ht.h>
#include <raikv/uint_ht.h>

namespace rai {
namespace aekv {

/* subscription route table element */
struct AeronSubRoute {
  uint32_t hash;       /* hash of subject */
  uint32_t sub;        /* the list of zipped routes for sub */
  uint16_t len;        /* length of subject string */
  char     value[ 2 ]; /* the subject string */
  bool equals( const void *s,  uint16_t l ) const {
    return l == this->len && ::memcmp( s, this->value, l ) == 0;
  }
  void copy( const void *s,  uint16_t l ) {
    ::memcpy( this->value, s, l );
  }
};
/* subscription route status */
enum AeronSubStatus {
  AERON_SUB_OK        = 0,
  AERON_SUB_NEW       = 1,
  AERON_SUB_EXISTS    = 2,
  AERON_SUB_NOT_FOUND = 3,
  AERON_SUB_REMOVED   = 4
};
/* subscription route table iterator */
struct AeronSubRoutePos {
  AeronSubRoute * rt;
  uint32_t v;
  uint16_t off;
};
/* subscription route table */
struct AeronSubMap {
  kv::RouteVec<AeronSubRoute> tab; /* ht of subject to zip ids */
  kv::RouteZip zip;                /* id hash to id array */

  bool is_null( void ) const {     /* if no subs */
    return this->tab.vec_size == 0;
  }
  size_t sub_count( void ) const { /* count of unique subs */
    return this->tab.pop_count();
  }
  void release( void ) {           /* free ht */
    this->tab.release();
  }
  /* add id to subject route */
  AeronSubStatus put( uint32_t h,  const char *sub,  size_t len, uint32_t id ) {
    kv::RouteLoc loc;
    AeronSubRoute * rt = this->tab.upsert( h, sub, len, loc );
    if ( rt == NULL )
      return AERON_SUB_NOT_FOUND;
    if ( loc.is_new ) {
      rt->sub = AeronSubMap::make_sub( id );
      return AERON_SUB_NEW;              /* if new subscription */
    }
    return AeronSubMap::merge_sub( this->zip, rt->sub, id );
  }
  /* remove id from subject route */
  AeronSubStatus rem( uint32_t h,  const char *sub,  size_t len, uint32_t id ) {
    kv::RouteLoc loc;
    AeronSubRoute * rt = this->tab.find( h, sub, len, loc );
    AeronSubStatus x;
    if ( rt == NULL )
      return AERON_SUB_NOT_FOUND;
    x = AeronSubMap::remove_sub( this->zip, rt->sub, id );
    if ( x == AERON_SUB_REMOVED )
      this->tab.remove( loc );
    return x;
  }
  /* iterate first tab[ sub ] */
  bool first( AeronSubRoutePos &pos ) {
    pos.rt = this->tab.first( pos.v, pos.off );
    return pos.rt != NULL;
  }
  /* iterate next tab[ sub ] */
  bool next( AeronSubRoutePos &pos ) {
    pos.rt = this->tab.next( pos.v, pos.off );
    return pos.rt != NULL;
  }
  /* after insert of first id in tab[ sub ] */
  static uint32_t make_sub( uint32_t i ) noexcept;
  /* merge insert id in tab[ sub ] */
  static AeronSubStatus merge_sub( kv::RouteZip &zip,  uint32_t &sub,
                                   uint32_t i ) noexcept;
  /* remove id in from tab[ sub ] */
  static AeronSubStatus remove_sub( kv::RouteZip &zip,  uint32_t &sub,
                                    uint32_t i ) noexcept;
  void print( void ) noexcept;
};
/* pattern sub route table element */
struct AeronPatternSubRoute {
  uint32_t hash;       /* hash of pattern prefix */
  uint32_t sub;        /* list of zipped routes for pattern */
  uint16_t len,        /* length of prefix and pattern */
           pref;       /* length of prefix */
  char     value[ 4 ]; /* prefix string and pattern string */
  bool equals( const void *s,  uint16_t ) const {
    return ::strcmp( (const char *) s, this->value ) == 0;
  }
  bool prefix_equals( const void *s,  uint16_t preflen ) const {
    return preflen == this->prefix_len() &&
           ::memcmp( s, this->prefix(), preflen ) == 0;
  }
  void copy( const void *s,  uint16_t l ) {
    ::memcpy( this->value, s, l );
  }
  const char *prefix( void ) const {
    return &this->value[ this->len - ( this->pref + 1 ) ];
  }
  size_t prefix_len( void ) const {
    return this->pref;
  }
  const char *pattern( void ) const {
    return this->value;
  }
  size_t pattern_len( void ) const {
    return this->len - ( this->pref + 2 );
  }
};
/* pattern sub route list elem */
struct AeronTmpElem {
  AeronTmpElem       * next,
                     * back;
  AeronPatternSubRoute x;    /* a pattern sub route element */
  void * operator new( size_t, void *ptr ) { return ptr; }
  AeronTmpElem( const AeronPatternSubRoute &rt )
      : next( 0 ), back( 0 ) {
    this->x.hash = rt.hash;
    this->x.len  = rt.len;
    this->x.pref = rt.pref;
    ::memcpy( this->x.value, rt.value, rt.len );
  }
  AeronTmpElem( const AeronSubRoute &rt )
      : next( 0 ), back( 0 ) {
    this->x.hash = rt.hash;
    this->x.len  = rt.len;
    this->x.pref = 0;
    ::memcpy( this->x.value, rt.value, rt.len );
  }
  static size_t alloc_size( const AeronPatternSubRoute &rt ) {
    return sizeof( AeronTmpElem ) + rt.len;
  }
  static size_t alloc_size( const AeronSubRoute &rt ) {
    return sizeof( AeronTmpElem ) + rt.len;
  }
};
/* pattern sub route list, used to remove several patterns at a time */
struct AeronTmpList {
  kv::WorkAllocT< 1024 > wrk;
  kv::DLinkList<AeronTmpElem> list;

  void append( const AeronPatternSubRoute &rt ) {
    void * p = this->wrk.alloc( AeronTmpElem::alloc_size( rt ) );
    this->list.push_tl( new ( p ) AeronTmpElem( rt ) );
  }
  void append( const AeronSubRoute &rt ) {
    void * p = this->wrk.alloc( AeronTmpElem::alloc_size( rt ) );
    this->list.push_tl( new ( p ) AeronTmpElem( rt ) );
  }
};
/* pattern sub route table iterator */
struct AeronPatternSubRoutePos {
  AeronPatternSubRoute * rt;
  uint32_t v;
  uint16_t off;
};
/* pattern sub route table */
struct AeronPatternSubMap {
  kv::RouteVec<AeronPatternSubRoute> tab; /* ht of sub pattern to zip ids */
  kv::RouteZip zip;                       /* id hash to id array */

  bool is_null( void ) const { /* test if no patterns are subscribed */
    return this->tab.vec_size == 0;
  }
  size_t sub_count( void ) const { /* return count of unique patterns */
    return this->tab.pop_count();
  }
  void release( void ) { /* release patterns table */
    this->tab.release();
  }
  /* add id to list of routes for a pattern prefix */
  AeronSubStatus put( uint32_t h,  const char *sub,  size_t len,
                      size_t pref,  uint32_t id ) {
    kv::RouteLoc loc;
    AeronPatternSubRoute * rt = this->tab.upsert( h, sub, len, loc );
    if ( rt == NULL )
      return AERON_SUB_NOT_FOUND;
    if ( loc.is_new ) {
      rt->pref = pref;
      rt->sub  = AeronSubMap::make_sub( id );
      return AERON_SUB_NEW;              /* if new subscription */
    }
    return AeronSubMap::merge_sub( this->zip, rt->sub, id );
  }
  /* remove id from list of routes for a pattern prefix */
  AeronSubStatus rem( uint32_t h,  const char *prefix,  size_t preflen,
                      uint32_t id,  AeronTmpList &tmp ) {
    kv::RouteLoc loc;
    AeronPatternSubRoute * rt = this->tab.find_by_hash( h, loc );
    if ( rt == NULL )
      return AERON_SUB_NOT_FOUND;
    do {
      if ( rt->prefix_equals( prefix, preflen ) ) {
        AeronSubStatus x;
        x = AeronSubMap::remove_sub( this->zip, rt->sub, id );
        if ( x == AERON_SUB_REMOVED )
          tmp.append( *rt );
      }
    } while ( (rt = this->tab.find_next_by_hash( h, loc )) != NULL );
    for ( AeronTmpElem *el = tmp.list.hd; el != NULL; el = el->next )
      this->tab.remove( h, el->x.value, el->x.len );
    return AERON_SUB_OK;
  }
  /* iterate first tab[ sub ] */
  bool first( AeronPatternSubRoutePos &pos ) {
    pos.rt = this->tab.first( pos.v, pos.off );
    return pos.rt != NULL;
  }
  /* iterate next tab[ sub ] */
  bool next( AeronPatternSubRoutePos &pos ) {
    pos.rt = this->tab.next( pos.v, pos.off );
    return pos.rt != NULL;
  }
  void print( void ) noexcept;
};

enum SessionState {
  SESSION_NEW      = 1, /* set initially, cleared after subs are sent */
  SESSION_DATALOSS = 2, /* when seqno is missing */
  SESSION_TIMEOUT  = 4, /* when timer expires after no heartbeats */
  SESSION_BYE      = 8  /* if session closes */
};

struct AeronSession {
  AeronSession * next,        /* link in MyPeers::list or MyPeers::free_list */
               * back,
               * next_id,     /* link in session_idx[] collision chain */
               * last_id;
  const uint64_t stamp;       /* identifies session uniquely */
  uint64_t       last_active, /* time in ns of last message recvd */
                 last_seqno,  /* seqno of last message recvd */
                 delta_seqno, /* if missing seqno, this is delta missing */
                 pub_count;   /* count of msgs published */
  const uint32_t id;          /* id is index into sessions[] */
  uint32_t       sub_count,   /* count of subscriptions */
                 psub_count,  /* count of pattern subs */
                 state;       /* state of session, bits of SessionState */

  void     set( SessionState fl )        { this->state |= (uint32_t) fl; }
  uint32_t test( SessionState fl ) const { return this->state & (uint32_t) fl; }
  void     clear( void )                 { this->state = 0; }
  void     clear( SessionState fl )      { this->state &= ~(uint32_t) fl; }

  void * operator new( size_t, void *ptr ) { return ptr; }
  AeronSession( uint32_t i,  uint64_t stmp = 0,  uint64_t seq = 0,
                AeronSession *nid = 0 )
    : next( 0 ), back( 0 ), next_id( nid ), last_id( 0 ), stamp( stmp ),
      last_active( 0 ), last_seqno( seq ), delta_seqno( 1 ), pub_count( 0 ),
      id( i ), sub_count( 0 ), psub_count( 0 ), state( SESSION_NEW ) {
    if ( nid != NULL )
      nid->last_id = this;
  }
};

struct MyPeers {
  kv::DLinkList<AeronSession>
                    list,           /* ordered list, hd = last active */
                    free_list;      /* free sessions */
  kv::UIntHashTab * session_idx;    /* idx of sessions[] */
  AeronSession    * last_session,   /* last sessions[] used */
                 ** sessions;       /* array of sessions */
  uint32_t          session_size;   /* size of net_ses[] array */
  AeronSession      dummy_session;  /* a null session */
  uint64_t          last_check_ns;  /* last timeout check */
  MyPeers() noexcept;

  static uint32_t hash( uint64_t stamp ) { /* hash of stamp */
    return (uint32_t) stamp ^ (uint32_t) ( stamp >> 32 );
  }
  /* find session and update last seqno seen */
  AeronSession *update_session( uint64_t stamp,  uint64_t seqno ) {
    if ( this->last_session->stamp == stamp )
      return this->update_last( seqno );

    uint32_t h = hash( stamp ), pos, id;
    if ( this->session_idx->find( h, pos, id ) ) {
      this->last_session = this->sessions[ id ];
      while ( this->last_session->stamp != stamp ) {
        this->last_session = this->last_session->next_id;
        if ( this->last_session == NULL )
          return this->new_session( stamp, seqno, h, pos, this->sessions[ id ]);
      }
      this->list.pop( this->last_session );
      this->list.push_hd( this->last_session );
      return this->update_last( seqno );
    }
    return this->new_session( stamp, seqno, h, pos, NULL );
  }
  /* update the last_session seen */
  AeronSession *update_last( uint64_t seqno ) {
    this->last_session->delta_seqno = seqno - this->last_session->last_seqno;
    if ( this->last_session->delta_seqno != 1 )
      this->last_session->set( SESSION_DATALOSS );
    else
      this->last_session->clear( SESSION_TIMEOUT );
    this->last_session->last_seqno = seqno; 
    return this->last_session;
  }
  /* allocate new session and insert into session_idx[] */
  AeronSession *new_session( uint64_t stamp,  uint64_t seqno,
                             uint32_t h,  uint32_t pos,
                             AeronSession *next_id ) noexcept;
  /* unlink session and put on free list */
  void release_session( AeronSession &session ) noexcept;
  /* check whether a session timed out */
  AeronSession *check_timeout( uint64_t age_ns ) {
    if ( age_ns > this->last_check_ns ) {
      this->last_check_ns = age_ns;
      if ( this->list.tl != NULL ) {
        if ( this->list.tl->last_active < age_ns ) {
          if ( this->list.tl->test( SESSION_TIMEOUT ) )
            return this->list.tl;
          this->list.tl->set( SESSION_TIMEOUT );
        }
      }
    }
    return NULL;
  }
  void print( void ) noexcept;
};

struct MySubs {
  kv::UIntHashTab * subsc_idx;   /* subscriptions active internal */
  uint32_t        * subs;        /* array of subscription msgs */
  uint32_t          subs_free,   /* count of free message words */
                    subs_cnt,    /* end of subs[] words array */
                    subs_size;   /* alloc words size of subs[] array */
  MySubs() noexcept;
  void gc( void ) noexcept;
  void upsert( kv::KvSubMsg &msg ) noexcept;
  void remove( kv::KvSubMsg &msg ) noexcept;
  void remove_pattern( kv::KvSubMsg &msg ) noexcept;
  uint32_t append( kv::KvSubMsg &msg ) noexcept;
  static uint32_t subs_align( uint32_t sz ) {
    return kv::align<uint32_t>( sz, 4 );
  }
  void print( kv::EvPoll &poll ) noexcept;
};

struct EvAeron : public kv::EvSocket, public kv::KvSendQueue,
                 public kv::RouteNotify {
  aeron_context_t            * context;
  aeron_t                    * aeron;
  aeron_publication_t        * pub;
  aeron_subscription_t       * sub;
  aeron_fragment_assembler_t * fragment_asm;
  uint64_t                     timer_id,
                               cur_mono_ns;
  AeronSubMap                  sub_tab;     /* active subscriptions on net */
  AeronPatternSubMap           pat_sub_tab; /* active wildcards on net */
  MyPeers                      my_peers;
  MySubs                       my_subs;

  void * operator new( size_t, void *ptr ) { return ptr; }
  EvAeron( kv::EvPoll &p ) noexcept;
  static EvAeron *create_aeron( kv::EvPoll &p ) noexcept;
  bool start_aeron( const char *pub_channel,  int pub_stream_id,
                    const char *sub_channel, int sub_stream_id ) noexcept;
  void release_aeron( void ) noexcept;

  /* EvSocket */
  virtual void write( void ) noexcept final;
  virtual void read( void ) noexcept final;
  virtual void process( void ) noexcept final;
  virtual void release( void ) noexcept final;
  virtual bool timer_expire( uint64_t timer_id, uint64_t event_id ) noexcept;
  virtual void process_shutdown( void ) noexcept final;
  virtual void process_close( void ) noexcept final;
  virtual bool on_msg( kv::EvPublish &pub ) noexcept;

  bool init_pubsub( const char *pub_channel,  int pub_stream_id,
                    const char *sub_channel,  int sub_stream_id ) noexcept;
  static void poll_handler( void *clientd,  const uint8_t *buffer,
                            size_t length,  aeron_header_t *header );
  void on_poll_handler( const uint8_t *buffer,  size_t length,
                        aeron_header_t *header ) noexcept;
  /* RouteNotify */
  virtual void on_sub( uint32_t h,  const char *sub,  size_t sublen,
                    uint32_t src_fd,  uint32_t rcnt,  char src_type,
                    const char *rep,  size_t rlen ) noexcept;
  virtual void on_unsub( uint32_t h,  const char *sub,  size_t sublen,
                    uint32_t src_fd,  uint32_t rcnt,  char src_type ) noexcept;
  virtual void on_psub( uint32_t h,  const char *pattern,  size_t patlen,
                    const char *prefix,  uint8_t prefix_len,
                    uint32_t src_fd,  uint32_t rcnt,  char src_type ) noexcept;
  virtual void on_punsub( uint32_t h,  const char *pattern,  size_t patlen,
                    const char *prefix,  uint8_t prefix_len,
                    uint32_t src_fd,  uint32_t rcnt,  char src_type ) noexcept;

  void publish_my_subs( void ) noexcept;
  void send_dataloss( AeronSession &session ) noexcept;
  void clear_session( AeronSession &session ) noexcept;
  void clear_subs( AeronSession &session ) noexcept;
  void clear_pattern_subs( AeronSession &session ) noexcept;
  void clear_all_subs( void ) noexcept;
  void print_stats( void ) noexcept;
};

}
}
#endif
