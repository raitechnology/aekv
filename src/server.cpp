#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <aekv/ev_aeron.h>
#include <raikv/mainloop.h>

using namespace rai;
using namespace aekv;
using namespace kv;

struct Args : public MainLoopVars { /* argv[] parsed args */
  Args() {}
};

struct Loop : public MainLoop<Args> {
  Loop( EvShm &m,  Args &args,  int num, bool (*ini)( void * ) ) :
    MainLoop<Args>( m, args, num, ini ) {}

 EvAeron * aeron_sv;

  bool aeron_init( void ) {
    this->aeron_sv = EvAeron::create_aeron( this->poll );
    if ( this->aeron_sv != NULL )
      return this->aeron_sv->start_aeron( NULL, "aeron:ipc", 100, "aeron:ipc", 100 );
    return false;
  }

  bool init( void ) {
    if ( this->aeron_init() )
      return true;
    return false;
  }

  static bool initialize( void *me ) noexcept {
    return ((Loop *) me)->init();
  }
};

int
main( int argc, const char *argv[] )
{
  EvShm shm;
  Args  r;

  if ( ! r.parse_args( argc, argv ) )
    return 1;
  if ( shm.open( r.map_name, r.db_num ) != 0 )
    return 1;
  shm.print();
  Runner<Args, Loop> runner( r, shm, Loop::initialize );
  if ( r.thr_error == 0 )
    return 0;
  return 1;
}

