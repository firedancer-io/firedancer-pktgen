#include <firedancer/util/fd_util.h>
#include <firedancer/util/log/fd_log.h>
#include <firedancer/util/tile/fd_tile.h>
#include <firedancer/util/wksp/fd_wksp.h>

#include "../cfg/fdgen_cfg_net.h"  /* fdgen_port_range_t */
#include "../tile/fdgen_tile_net_xsk.h"  /* fdgen_tile_net_xsk_run */

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  /* Collect arguments */

  ulong cpu_idx = fd_tile_cpu_id( fd_tile_idx() );
  if( cpu_idx>=fd_shmem_cpu_cnt() ) cpu_idx = 0UL;

  char const * _page_sz   = fd_env_strip_cmdline_cstr ( &argc, &argv, "--page-sz",  NULL, "gigantic"                 );
  ulong        page_cnt   = fd_env_strip_cmdline_ulong( &argc, &argv, "--page-cnt", NULL, 2UL                        );
  ulong        numa_idx   = fd_env_strip_cmdline_ulong( &argc, &argv, "--numa-idx", NULL, fd_shmem_numa_idx(cpu_idx) );
  char const * iface      = fd_env_strip_cmdline_cstr ( &argc, &argv, "--iface",    NULL, NULL                       );
  char const * _net_mode  = fd_env_strip_cmdline_cstr ( &argc, &argv, "--net-mode", NULL, "xdp"                      );
  ulong        pod_sz     = fd_env_strip_cmdline_ulong( &argc, &argv, "--pod-sz",   NULL, 0x10000UL                  );
  char const * _src_ports = fd_env_strip_cmdline_cstr ( &argc, &argv, "--src-port", NULL, "9000"                     );

  /* Parse arguments */

  ulong page_sz = fd_cstr_to_shmem_page_sz( _page_sz );
  if( FD_UNLIKELY( !page_sz ) ) FD_LOG_ERR(( "unsupported --page-sz" ));

  if( FD_UNLIKELY( !iface ) ) FD_LOG_ERR(( "Missing --iface" ));

  int net_mode = fdgen_cstr_to_net_mode( _net_mode );
  if( FD_UNLIKELY( !net_mode ) ) FD_LOG_ERR(( "Invalid --net-mode" ));

  fdgen_port_range_t src_ports[1];
  if( FD_UNLIKELY( !fdgen_cstr_to_port_range( src_ports, (char *)_src_ports ) ) ) {
    FD_LOG_ERR(( "Invalid --src-ports" ));
  }
  FD_LOG_NOTICE(( "Using UDP source port range [%u,%u)", src_ports->lo, src_ports->hi ));

  /* Allocate workspace */

  FD_LOG_NOTICE(( "Creating workspace with --page-cnt %lu --page-sz %s pages on --numa-idx %lu", page_cnt, _page_sz, numa_idx ));
  fd_wksp_t * wksp = fd_wksp_new_anonymous( page_sz, page_cnt, fd_shmem_cpu_idx( numa_idx ), "wksp", 0UL );
  FD_TEST( wksp );

  /* Copy config into pod */

  uchar * pod = fd_pod_join( fd_pod_new( fd_wksp_alloc_laddr( wksp, FD_POD_ALIGN, pod_sz, 1UL ), pod_sz ) );

  ulong next_tile_idx = fd_tile_idx() + 1UL;

  char * tile_args[3] = { (char *)wksp, (char *)pod, NULL };
  FD_TEST( fd_tile_exec_new( 1UL, fdgen_tile_net_xsk_run, 2, tile_args ) );
  fd_log_sleep( (ulong)1e9 * 7200 );

  /* Clean up */

  fd_wksp_delete_anonymous( wksp );
  fd_halt();
  return 0;
}
