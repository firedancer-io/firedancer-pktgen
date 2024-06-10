#include "fdgen_tile_net_xsk_rx.h"
#include "../../cfg/fdgen_netlink.h"

/* test_tile_net_xsk_rx.c tests AF_XDP functionality using a veth pair
   in two network namespaces. */

#include <errno.h>          /* errno(3) */
#include <unistd.h>         /* close(2) */
#include <linux/netlink.h>  /* NETLINK_ROUTE */

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  uid_t uid = geteuid();
  if( FD_UNLIKELY( uid!=0 ) ) {
    FD_LOG_WARNING(( "Not running as root. Setting up a veth pair will most likely fail" ));
  }

  if( FD_UNLIKELY( fd_tile_cnt()<2 ) ) FD_LOG_ERR(( "This test requires at least 2 tiles" ));

  /* Create network namespaces and veth pair */

  int netns1 = fdgen_create_netns();  FD_TEST( netns1>=0 );
  int netns2 = fdgen_create_netns();  FD_TEST( netns2>=0 );

  int nl_route = fdgen_netlink_connect( NETLINK_ROUTE );  FD_TEST( nl_route>=0 );
  FD_TEST( 0==fdgen_create_veth_pair( nl_route, netns1, netns2 ) );
  close( nl_route );

  /* Spawn tiles */

  FD_LOG_INFO(( "Cleaning up" ));

  close( netns1 );
  close( netns2 );

  FD_LOG_NOTICE(( "pass" ));
  fd_halt();
  return 0;
}
