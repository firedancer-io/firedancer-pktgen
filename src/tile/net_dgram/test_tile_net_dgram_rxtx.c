#include "fdgen_tile_net_dgram_rxtx.h"
#include <firedancer/tango/cnc/fd_cnc.h>
#include <firedancer/tango/mcache/fd_mcache.h>
#include <firedancer/tango/dcache/fd_dcache.h>
#include <firedancer/tango/tempo/fd_tempo.h>

#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>

struct test_rxtx_args {
  fd_wksp_t * wksp;
  fd_cnc_t *  cnc;
  long        lazy;
  ulong       mtu;
  uint        seed;

  fd_frag_meta_t * tx_mcache;
  fd_frag_meta_t * rx_mcache;
  uchar *          rx_dcache;

  ulong tx_burst;
  long  tx_burst_timeout;
  ulong rx_burst;
};

typedef struct test_rxtx_args test_rxtx_args_t;

static int
rxtx_tile_main( int     argc,
                char ** argv ) {

  assert( argc==1 );
  test_rxtx_args_t * args = fd_type_pun( argv[0] );

  fd_rng_t  _rng[1];
  fd_rng_t * rng = fd_rng_join( fd_rng_new( _rng, args->seed, 0UL ) );

  int epoll_fd = epoll_create1( 0 );
  if( FD_UNLIKELY( !epoll_fd ) ) {
    FD_LOG_WARNING(( "epoll_create1 failed (%i-%s)", errno, fd_io_strerror( errno ) ));
    return 1;
  }

  ulong   rx_depth   = fd_mcache_depth( args->rx_mcache );
  ulong   scratch_sz = fdgen_tile_net_dgram_scratch_footprint( rx_depth, args->rx_burst, args->tx_burst, args->mtu );
  uchar * scratch    = fd_wksp_alloc_laddr( args->wksp, fdgen_tile_net_dgram_scratch_align(), scratch_sz, 1UL );

  double tick_per_ns = fd_tempo_tick_per_ns( NULL );
  fdgen_tile_net_dgram_rxtx_cfg_t cfg[1] = {{
    .orig        = 0UL,
    .lazy        = args->lazy,
    .tick_per_ns = tick_per_ns,
    .seq0        = 0UL,  /* FIXME test with non-zero */
    .mtu         = args->mtu,

    .rng       = rng,
    .cnc       = args->cnc,
    .tx_base   = (void *)args->wksp,
    .tx_mcache = args->tx_mcache,
    .rx_base   = (void *)args->wksp,
    .rx_mcache = args->rx_mcache,
    .rx_dcache = args->rx_dcache,

    .tx_burst         = args->tx_burst,
    .tx_burst_timeout = args->tx_burst_timeout,
    .rx_burst         = args->rx_burst,

    .epoll_fd = epoll_fd,
    .send_fd  = -1,

    .scratch    = scratch,
    .scratch_sz = scratch_sz,
  }};

  int res = fdgen_tile_net_dgram_rxtx_run( cfg );

  close( epoll_fd );
  return res;
}

static int
send_tile_main( int     argc,
                char ** argv ) {
  return 0;
}

static int
recv_tile_main( int     argc,
                char ** argv ) {
  return 0;
}

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  ulong cpu_idx = fd_tile_cpu_id( fd_tile_idx() );
  if( cpu_idx>=fd_shmem_cpu_cnt() ) cpu_idx = 0UL;

  char const * _page_sz = fd_env_strip_cmdline_cstr ( &argc, &argv, "--page-sz",      NULL, "gigantic"                 );
  ulong        page_cnt = fd_env_strip_cmdline_ulong( &argc, &argv, "--page-cnt",     NULL, 1UL                        );
  ulong        numa_idx = fd_env_strip_cmdline_ulong( &argc, &argv, "--numa-idx",     NULL, fd_shmem_numa_idx(cpu_idx) );
  ulong        rx_depth = fd_env_strip_cmdline_ulong( &argc, &argv, "--rx-depth",     NULL, 1024UL                     );
  ulong        tx_depth = fd_env_strip_cmdline_ulong( &argc, &argv, "--tx-depth",     NULL, 1024UL                     );
  ulong        rx_burst = fd_env_strip_cmdline_ulong( &argc, &argv, "--rx-burst",     NULL,  128UL                     );
  ulong        tx_burst = fd_env_strip_cmdline_ulong( &argc, &argv, "--tx-burst",     NULL,  128UL                     );
  ulong        mtu      = fd_env_strip_cmdline_ulong( &argc, &argv, "--mtu",          NULL, 1500UL                     );
  uint         seed     = fd_env_strip_cmdline_uint ( &argc, &argv, "--seed",         NULL,    0U                      );

  ulong page_sz = fd_cstr_to_shmem_page_sz( _page_sz );
  if( FD_UNLIKELY( !page_sz ) ) FD_LOG_ERR(( "unsupported --page-sz" ));

  if( FD_UNLIKELY( fd_tile_cnt()<4 ) ) FD_LOG_ERR(( "This test requires at least 4 tiles" ));

  FD_LOG_NOTICE(( "Creating workspace with --page-cnt %lu --page-sz %s pages on --numa-idx %lu", page_cnt, _page_sz, numa_idx ));
  fd_wksp_t * wksp = fd_wksp_new_anonymous( page_sz, page_cnt, fd_shmem_cpu_idx( numa_idx ), "wksp", 0UL );
  FD_TEST( wksp );

  /* Allocate objects */

  void *     rxtx_cnc_mem = fd_wksp_alloc_laddr( wksp, fd_cnc_align(), fd_cnc_footprint( 64UL ), 1UL );
  fd_cnc_t * rxtx_cnc     = fd_cnc_join( fd_cnc_new( rxtx_cnc_mem, 64UL, 1UL, fd_tickcount() ) );
  FD_TEST( rxtx_cnc );

  void *     send_cnc_mem = fd_wksp_alloc_laddr( wksp, fd_cnc_align(), fd_cnc_footprint( 64UL ), 1UL );
  fd_cnc_t * send_cnc     = fd_cnc_join( fd_cnc_new( send_cnc_mem, 64UL, 1UL, fd_tickcount() ) );
  FD_TEST( send_cnc );

  void *     recv_cnc_mem = fd_wksp_alloc_laddr( wksp, fd_cnc_align(), fd_cnc_footprint( 64UL ), 1UL );
  fd_cnc_t * recv_cnc     = fd_cnc_join( fd_cnc_new( recv_cnc_mem, 64UL, 1UL, fd_tickcount() ) );
  FD_TEST( recv_cnc );

  void *           rx_mcache_mem = fd_wksp_alloc_laddr( wksp, fd_mcache_align(), fd_mcache_footprint( rx_depth, 0UL ), 1UL );
  fd_frag_meta_t * rx_mcache     = fd_mcache_join( fd_mcache_new( rx_mcache_mem, rx_depth, 0UL, /* seq0 */ 0UL ) );
  FD_TEST( rx_mcache );

  ulong   rx_dcache_data_sz = fdgen_tile_net_dgram_dcache_data_sz( rx_depth, rx_burst, mtu );
  void *  rx_dcache_mem     = fd_wksp_alloc_laddr( wksp, fd_dcache_align(), fd_dcache_footprint( rx_dcache_data_sz, 0UL ), 1UL );
  uchar * rx_dcache         = fd_dcache_join( fd_dcache_new( rx_dcache_mem, rx_dcache_data_sz, 0UL ) );

  void *           tx_mcache_mem = fd_wksp_alloc_laddr( wksp, fd_mcache_align(), fd_mcache_footprint( tx_depth, 0UL ), 1UL );
  fd_frag_meta_t * tx_mcache     = fd_mcache_join( fd_mcache_new( tx_mcache_mem, tx_depth, 0UL, /* seq0 */ 0UL ) );
  FD_TEST( tx_mcache );

  /* Spawn tiles */

  test_rxtx_args_t rxtx_args = {
    .wksp    = wksp,
    .cnc     = rxtx_cnc,
    .lazy    = 100,  /* TODO */
    .mtu     = mtu,
    .seed    = seed + 1U,

    .tx_mcache = tx_mcache,
    .tx_burst  = tx_burst,
    .rx_mcache = rx_mcache,
    .rx_dcache = rx_dcache,
    .rx_burst  = rx_burst
  };

  char * rx_tile_argv[1] = { fd_type_pun( &rxtx_args ) };

  fd_tile_exec_t * rxtx_tile = fd_tile_exec_new( 1UL, rxtx_tile_main, 1, rx_tile_argv );
  fd_tile_exec_t * send_tile = fd_tile_exec_new( 2UL, send_tile_main, 0, NULL );
  fd_tile_exec_t * recv_tile = fd_tile_exec_new( 3UL, recv_tile_main, 0, NULL );

  FD_TEST( fd_cnc_wait( rxtx_cnc, FD_CNC_SIGNAL_BOOT, (long)5e9, NULL )==FD_CNC_SIGNAL_RUN );
  FD_TEST( fd_cnc_wait( send_cnc, FD_CNC_SIGNAL_BOOT, (long)5e9, NULL )==FD_CNC_SIGNAL_RUN );
  FD_TEST( fd_cnc_wait( recv_cnc, FD_CNC_SIGNAL_BOOT, (long)5e9, NULL )==FD_CNC_SIGNAL_RUN );

  FD_LOG_INFO(( "Cleaning up" ));

  FD_TEST( !fd_cnc_open( rxtx_cnc ) );
  FD_TEST( !fd_cnc_open( send_cnc ) );
  FD_TEST( !fd_cnc_open( recv_cnc ) );

  fd_cnc_signal( rxtx_cnc, FD_CNC_SIGNAL_HALT );
  fd_cnc_signal( send_cnc, FD_CNC_SIGNAL_HALT );
  fd_cnc_signal( recv_cnc, FD_CNC_SIGNAL_HALT );

  fd_cnc_close( rxtx_cnc );
  fd_cnc_close( send_cnc );
  fd_cnc_close( recv_cnc );

  FD_TEST( fd_cnc_wait( rxtx_cnc, FD_CNC_SIGNAL_BOOT, (long)5e9, NULL )==FD_CNC_SIGNAL_BOOT );
  FD_TEST( fd_cnc_wait( send_cnc, FD_CNC_SIGNAL_BOOT, (long)5e9, NULL )==FD_CNC_SIGNAL_BOOT );
  FD_TEST( fd_cnc_wait( recv_cnc, FD_CNC_SIGNAL_BOOT, (long)5e9, NULL )==FD_CNC_SIGNAL_BOOT );

  fd_tile_exec_delete( rxtx_tile, NULL );
  fd_tile_exec_delete( send_tile, NULL );
  fd_tile_exec_delete( recv_tile, NULL );

  fd_wksp_free_laddr( fd_cnc_delete( fd_cnc_leave( rxtx_cnc ) ) );
  fd_wksp_free_laddr( fd_cnc_delete( fd_cnc_leave( send_cnc ) ) );
  fd_wksp_free_laddr( fd_cnc_delete( fd_cnc_leave( recv_cnc ) ) );

  fd_wksp_delete_anonymous( wksp );

  FD_LOG_NOTICE(( "pass" ));
  fd_halt();
  return 0;
}

