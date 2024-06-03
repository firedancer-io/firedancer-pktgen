#define FD_HAS_THREADS 1
#define FD_HAS_ATOMIC 1
#include <firedancer/util/fd_util.h>

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );
  fd_halt();
  return 0;
}
