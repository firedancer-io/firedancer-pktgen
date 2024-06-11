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

  ulong cpu_idx = fd_tile_cpu_id( fd_tile_idx() );
  if( cpu_idx>=fd_shmem_cpu_cnt() ) cpu_idx = 0UL;

  char const * _page_sz = fd_env_strip_cmdline_cstr ( &argc, &argv, "--page-sz",  NULL, "gigantic"                 );
  ulong        page_cnt = fd_env_strip_cmdline_ulong( &argc, &argv, "--page-cnt", NULL, 1UL                        );
  ulong        numa_idx = fd_env_strip_cmdline_ulong( &argc, &argv, "--numa-idx", NULL, fd_shmem_numa_idx(cpu_idx) );

  ulong page_sz = fd_cstr_to_shmem_page_sz( _page_sz );
  if( FD_UNLIKELY( !page_sz ) ) FD_LOG_ERR(( "unsupported --page-sz" ));

  if( FD_UNLIKELY( fd_tile_cnt()<2 ) ) FD_LOG_ERR(( "This test requires at least 2 tiles" ));

  FD_LOG_NOTICE(( "Creating workspace with --page-cnt %lu --page-sz %s pages on --numa-idx %lu", page_cnt, _page_sz, numa_idx ));
  fd_wksp_t * wksp = fd_wksp_new_anonymous( page_sz, page_cnt, fd_shmem_cpu_idx( numa_idx ), "wksp", 0UL );
  FD_TEST( wksp );

  /* Startup checks */

  uid_t uid = geteuid();
  if( FD_UNLIKELY( uid!=0 ) ) {
    FD_LOG_WARNING(( "Not running as root. Setting up a veth pair will most likely fail" ));
  }

  /* Create netns & veth pair */

  fdgen_veth_env_t veth_env[1] =
    {{ .rx_queue_cnt = {1, 1},
       .tx_queue_cnt = {1, 1} }};

  fdgen_netlink_create_veth_env( veth_env );

  /* Spawn tiles */

  FD_LOG_INFO(( "Cleaning up" ));

  close( veth_env->params[0].netns );
  close( veth_env->params[1].netns );

  fd_wksp_delete_anonymous( wksp );

  FD_LOG_NOTICE(( "pass" ));
  fd_halt();
  return 0;
}
