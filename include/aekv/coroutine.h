#ifndef __rai_aekv__coroutine_h__
#define __rai_aekv__coroutine_h__

/* derived from: https://github.com/cloudwu/coroutine */

#ifdef __cplusplus
extern "C" {
#endif

enum {
  COROUTINE_DEAD    = 0,
  COROUTINE_READY   = 1,
  COROUTINE_RUNNING = 2,
  COROUTINE_SUSPEND = 3
};

struct schedule_s;
struct coroutine_s;
typedef struct schedule_s schedule_t;
typedef struct coroutine_s coroutine_t;

typedef void ( *coroutine_func_t )( coroutine_t *, void *ud );

schedule_t * coroutine_open( void );
void coroutine_close( schedule_t *sched );

coroutine_t * coroutine_new( schedule_t *sched, coroutine_func_t f,
                             void *user_data,  const char *name );
void coroutine_resume( coroutine_t *co );
int  coroutine_status( coroutine_t *co );
coroutine_t * coroutine_running( schedule_t *sched );
void coroutine_yield( coroutine_t *co );
const char *coroutine_name( coroutine_t *co );

#ifdef __cplusplus
}
#endif

#endif
