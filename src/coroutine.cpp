
/* derived from: https://github.com/cloudwu/coroutine */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <aekv/coroutine.h>

#if __APPLE__ && __MACH__
#include <sys/ucontext.h>
#else 
#include <ucontext.h>
#endif 

static const size_t STACK_SIZE = (10*1024*1024);

extern "C" {
typedef struct coroutine_s {
  coroutine_func_t func;
  void           * ud;
  ucontext_t       ctx;
  schedule_t     * sch;
  size_t           cap,
                   size;
  char           * stack;
  size_t           id;
  const char     * name;
  int              status;
} coroutine_t;

typedef struct schedule_s {
  char           stack[ STACK_SIZE ] __attribute__((__aligned__( 128 )));
  ucontext_t     main;
  size_t         nco,
                 cap,
                 used;
  coroutine_t  * running;
  coroutine_t ** co;
} schedule_t;
}

static inline void *aligned_malloc( size_t sz ) {
#ifdef _ISOC11_SOURCE
  return ::aligned_alloc( 128, sz ); /* >= RH7 */
#else
  return ::memalign( 128, sz ); /* RH5, RH6.. */
#endif
}

struct Coroutine : public coroutine_s {
  void * operator new( size_t, void *ptr ) { return ptr; }
  void operator delete( void *ptr ) { free( ptr ); }
  Coroutine( schedule_t *s, coroutine_func_t f, void *user_data, size_t i,
             const char *nm,  char *stk = NULL,  size_t stk_cap = 0 ) {
    this->func   = f;
    this->ud     = user_data;
    this->sch    = s;
    this->cap    = stk_cap;
    this->size   = 0;
    this->stack  = stk;
    this->id     = i;
    this->name   = nm;
    this->status = COROUTINE_READY;
  }
  ~Coroutine() {
    if ( this->stack != NULL ) free( this->stack );
  }
};

struct Schedule : public schedule_s {
  void * operator new( size_t, void *ptr ) { return ptr; }
  void operator delete( void *ptr ) { free( ptr ); }
  Schedule() {
    this->nco     = 0;
    this->cap     = 0;
    this->used    = 0;
    this->running = NULL;
    this->co      = NULL;
  }
  ~Schedule() {
    for ( size_t i = 0; i < this->cap; i++ ) {
      if ( this->co[ i ] != NULL )
        delete (Coroutine *) this->co[ i ];
    }
    free( this->co );
  }
  Coroutine *new_coroutine( coroutine_func_t func, void *user_data,
                            const char *name ) {
    if ( this->used == this->cap )
      this->resize_coro( this->cap + 16 );

    size_t j = this->nco;
    Coroutine * c = NULL;
    for ( size_t i = 0; i < this->cap; i++ ) {
      if ( j >= this->cap )
        j = 0;
      c = (Coroutine *) this->co[ j ];
      if ( c == NULL || c->status == COROUTINE_DEAD )
        break;
    }
    this->nco = j + 1;
    this->used++;
    if ( c == NULL ) {
      void *p = malloc( sizeof( *c ) );
      c = new ( p ) Coroutine( this, func, user_data, j, name );
      this->co[ j ] = c;
      return c;
    }
    return new ( c ) Coroutine( this, func, user_data, j, name,
                                c->stack, c->cap );
  }
  void resize_coro( size_t n ) {
    size_t cur = this->cap * sizeof( this->co[ 0 ] ),
           sz  = n * sizeof( this->co[ 0 ] );
    this->co = (coroutine_t **) realloc( this->co, sz );
    memset( &this->co[ this->cap ], 0, sz - cur );
    this->cap = n;
  }
};

extern "C" {

schedule_t *
coroutine_open( void )
{
  return new ( aligned_malloc( sizeof( Schedule ) ) ) Schedule();
}

void
coroutine_close( schedule_t *sched )
{
  delete (Schedule *) sched;
}

coroutine_t *
coroutine_new( schedule_t *sched, coroutine_func_t func, void *user_data,
               const char *name )
{
  return ((Schedule *) sched)->new_coroutine( func, user_data, name );
}

static uint32_t uint_upper( void *p ) { return ((uintptr_t) p ) >> 32; }
static uint32_t uint_lower( void *p ) { return ((uintptr_t) p ); }
static void * uint_toptr( uintptr_t i,  uintptr_t j ) {
  return (void *) ( ( i << 32 ) | j );
}

static void
mainfunc( uint32_t i,  uint32_t j )
{
  Coroutine * c = (Coroutine *) uint_toptr( i, j );
  Schedule  * s = (Schedule *) c->sch;
  c->func( c, c->ud );
  c->status = COROUTINE_DEAD;
  s->used--;
  s->running = NULL;
}

void
coroutine_resume( coroutine_t *co )
{
  Coroutine * c = (Coroutine *) co;
  Schedule  * s = (Schedule *) c->sch;

  switch ( c->status ) {
    case COROUTINE_READY:
      getcontext( &c->ctx );
      c->ctx.uc_stack.ss_sp   = s->stack;
      c->ctx.uc_stack.ss_size = STACK_SIZE;
      c->ctx.uc_link          = &s->main;
      c->sch->running         = c;
      c->status               = COROUTINE_RUNNING;
      makecontext( &c->ctx, (void ( * )( void )) mainfunc, 2,
                   uint_upper( c ), uint_lower( c ) );
      swapcontext( &s->main, &c->ctx );
      break;

    case COROUTINE_SUSPEND:
      memcpy( s->stack + STACK_SIZE - c->size, c->stack, c->size );
      s->running = c;
      c->status  = COROUTINE_RUNNING;
      swapcontext( &s->main, &c->ctx );
      break;

    default:
      assert( 0 );
  }
}

static void
_save_stack( Coroutine *c, char *top )
{
  char dummy = 0;
  if ( c->cap < (size_t) ( top - &dummy ) ) {
    c->cap   = top - &dummy;
    c->stack = (char *) realloc( c->stack, c->cap );
  }
  c->size = top - &dummy;
  memcpy( c->stack, &dummy, c->size );
}

void
coroutine_yield( coroutine_t *co )
{
  Coroutine * c = (Coroutine *) co;
  Schedule  * s = (Schedule *) c->sch;

  _save_stack( c, s->stack + STACK_SIZE );
  c->status  = COROUTINE_SUSPEND;
  s->running = NULL;
  swapcontext( &c->ctx, &s->main );
}

int
coroutine_status( coroutine_t *co )
{
  return co->status;
}

coroutine_t *
coroutine_running( schedule_t *sched )
{
  return sched->running;
}

const char *
coroutine_name( coroutine_t *co )
{
  return co->name;
}
}
