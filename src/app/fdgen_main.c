#include <firedancer/util/bits/fd_bits.h>
#include <firedancer/util/fd_util.h>
#include <firedancer/util/log/fd_log.h>
#include <firedancer/util/tile/fd_tile.h>
#include <firedancer/util/wksp/fd_wksp.h>

#include "../cfg/fdgen_cfg_net.h"  /* fdgen_port_range_t */
#include "../tile/net_xsk/fdgen_tile_net_xsk_rx.h"  /* fdgen_tile_net_xsk_run */

#include <errno.h>          /* errno(3) */
#include <linux/if_link.h>
#include <sched.h>          /* setns(2) */
#include <time.h>
#include <unistd.h>         /* close(2) */
#include <linux/if_xdp.h>   /* xdp_{...} */
#include <linux/netlink.h>  /* NETLINK_ROUTE */
#include <net/if.h>         /* if_nametoindex */
#include <netinet/in.h>     /* sockaddr_in */
#include <sys/mman.h>       /* mmap(2) */

#include "../tile/net_xsk/fdgen_tile_net_xsk.h"
#include "../tile/net_xsk/fdgen_tile_net_xsk_rx.h"
#include "../tile/net_xsk/fdgen_tile_net_xsk_poll.h"
#include "../cfg/fdgen_cfg_net_xdp.h"
#include <firedancer/tango/cnc/fd_cnc.h>
#include <firedancer/tango/mcache/fd_mcache.h>
#include <firedancer/tango/dcache/fd_dcache.h>
#include <firedancer/tango/tempo/fd_tempo.h>
#include <firedancer/waltz/ebpf/fd_ebpf.h>
#include <firedancer/waltz/xdp/fd_xsk.h>
#include <firedancer/util/net/fd_eth.h>
#include <firedancer/util/net/fd_ip4.h>
#include <firedancer/util/net/fd_udp.h>

static int
poll_tile_main( int     argc,
                char ** argv ) {
  fdgen_tile_net_xsk_poll_cfg_t * cfg = fd_type_pun( argv[0] );
  return fdgen_tile_net_xsk_poll_run( cfg );
}

static int
rx_tile_main( int     argc,
              char ** argv ) {
  fdgen_tile_net_xsk_rx_cfg_t * cfg = fd_type_pun( argv[0] );
  return fdgen_tile_net_xsk_rx_run( cfg );
}

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  fd_rng_t _rng[1]; fd_rng_t * rng = fd_rng_join( fd_rng_new( _rng, fd_tickcount(), 0UL ) );

  /* Collect arguments */

  ulong cpu_idx = fd_tile_cpu_id( fd_tile_idx() );
  if( cpu_idx>=fd_shmem_cpu_cnt() ) cpu_idx = 0UL;

  char const * _page_sz         = fd_env_strip_cmdline_cstr ( &argc, &argv, "--page-sz",          NULL, "gigantic"                 );
  ulong        page_cnt         = fd_env_strip_cmdline_ulong( &argc, &argv, "--page-cnt",         NULL, 2UL                        );
  ulong        numa_idx         = fd_env_strip_cmdline_ulong( &argc, &argv, "--numa-idx",         NULL, fd_shmem_numa_idx(cpu_idx) );
  char const * iface            = fd_env_strip_cmdline_cstr ( &argc, &argv, "--iface",            NULL, NULL                       );
  char const * _net_mode        = fd_env_strip_cmdline_cstr ( &argc, &argv, "--net-mode",         NULL, "xdp"                      );
  //ulong        pod_sz         = fd_env_strip_cmdline_ulong( &argc, &argv, "--pod-sz",           NULL, 0x10000UL                  );
  char const * _src_ports       = fd_env_strip_cmdline_cstr ( &argc, &argv, "--src-port",         NULL, "9000"                     );
  ulong        busy_poll_usecs  = fd_env_strip_cmdline_ulong( &argc, &argv, "--busy-poll-usecs",  NULL,     50UL                   );
  ulong        xsk_burst        = fd_env_strip_cmdline_ulong( &argc, &argv, "--xsk-burst",        NULL,     64UL                   );
  ulong        rx_depth         = fd_env_strip_cmdline_ulong( &argc, &argv, "--rx-depth",         NULL,   4096UL                   );
  ulong        busy_poll_budget = fd_env_strip_cmdline_ulong( &argc, &argv, "--busy-poll-budget", NULL,   2048UL                   );
  char const * poll_mode_cstr   = fd_env_strip_cmdline_cstr ( &argc, &argv, "--poll-mode",        NULL, "wakeup"                   );

  int poll_mode = 0;
  if( 0==strcmp( poll_mode_cstr, "none" ) ) {
    poll_mode = FDGEN_XSK_POLL_MODE_NONE;
  } else if( 0==strcmp( poll_mode_cstr, "wakeup" ) ) {
    poll_mode = FDGEN_XSK_POLL_MODE_WAKEUP;
  } else if( 0==strcmp( poll_mode_cstr, "busy" ) ) {
    poll_mode = FDGEN_XSK_POLL_MODE_BUSY_SYNC;
  } else if( 0==strcmp( poll_mode_cstr, "busy-ext" ) ) {
    poll_mode = FDGEN_XSK_POLL_MODE_BUSY_EXT;
  } else {
    FD_LOG_ERR(( "invalid poll mode (%s)", poll_mode_cstr ));
  }

  FD_LOG_NOTICE(( "--rx-depth %lu", rx_depth ));
  FD_LOG_NOTICE(( "--poll-mode %s", poll_mode_cstr ));
  if( poll_mode==FDGEN_XSK_POLL_MODE_WAKEUP || poll_mode==FDGEN_XSK_POLL_MODE_BUSY_EXT ) {
    FD_LOG_NOTICE(( "--xsk-burst %lu", xsk_burst ));
  }
  if( poll_mode==FDGEN_XSK_POLL_MODE_BUSY_SYNC || poll_mode==FDGEN_XSK_POLL_MODE_BUSY_EXT ) {
    FD_LOG_NOTICE(( "--busy-poll-usecs %lu",  busy_poll_usecs ));
    FD_LOG_NOTICE(( "--busy-poll-budget %lu", busy_poll_budget ));
  }

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

  /* ???? */

  FD_TEST( fd_ulong_is_pow2( rx_depth ) );

  ulong depth    = 4096UL;
  ulong mtu      = 2048UL;
  ulong fr_depth = rx_depth<<1;

  void *     rx_cnc_mem = fd_wksp_alloc_laddr( wksp, fd_cnc_align(), fd_cnc_footprint( 64UL ), 1UL );
  fd_cnc_t * rx_cnc     = fd_cnc_join( fd_cnc_new( rx_cnc_mem, 64UL, 1UL, fd_tickcount() ) );
  FD_TEST( rx_cnc );

  void *     poll_cnc_mem = fd_wksp_alloc_laddr( wksp, fd_cnc_align(), fd_cnc_footprint( 64UL ), 1UL );
  fd_cnc_t * poll_cnc     = fd_cnc_join( fd_cnc_new( poll_cnc_mem, 64UL, 1UL, fd_tickcount() ) );
  FD_TEST( poll_cnc );

  if( FD_UNLIKELY( !fd_mcache_footprint( depth, 0UL ) ) ) FD_LOG_ERR(( "invalid depth" ));
  ulong            seq0       = 0UL;
  void *           mcache_mem = fd_wksp_alloc_laddr( wksp, fd_mcache_align(), fd_mcache_footprint( depth, 0UL ), 1UL );
  fd_frag_meta_t * mcache     = fd_mcache_join( fd_mcache_new( mcache_mem, depth, 0UL, seq0 ) );
  FD_TEST( mcache );

  if( FD_UNLIKELY( mtu!=2048 && mtu!=4096 ) ) FD_LOG_ERR(( "invalid mtu" ));
  ulong   dcache_depth   = depth + fr_depth;
  ulong   dcache_data_sz = mtu * dcache_depth;
  void *  dcache_mem     = fd_wksp_alloc_laddr( wksp, FD_DCACHE_ALIGN, fd_dcache_footprint( dcache_data_sz, 0UL ) + FD_XSK_UMEM_ALIGN, 1UL );
  uchar * dcache         = fd_dcache_join( fd_dcache_new( dcache_mem, dcache_data_sz, 0UL ) );
  FD_TEST( dcache );

  uint if_idx = if_nametoindex( iface );
  FD_TEST( if_idx );

  fdgen_xdp_port_redir_t _redir[1];
  fdgen_xdp_port_redir_t * redir = fdgen_xdp_full_redir_init(
     _redir, 1UL,
     if_idx, 0 );
  FD_TEST( redir );

  int xsk_fd = socket( AF_XDP, SOCK_RAW, 0 );
  FD_TEST( xsk_fd>=0 );

  ulong dcache_lo = (ulong)dcache;
  ulong dcache_hi = (ulong)dcache + fd_dcache_data_sz( dcache );
        dcache_lo = fd_ulong_align_up( dcache_lo, FD_XSK_UMEM_ALIGN );
        dcache_hi = fd_ulong_align_dn( dcache_hi, FD_XSK_UMEM_ALIGN );
  FD_TEST( dcache_lo < dcache_hi );

  struct xdp_umem_reg umem =
    { .headroom   = 0U,
      .addr       = dcache_lo,
      .chunk_size = mtu,
      .len        = dcache_hi - dcache_lo };

  FD_LOG_INFO(( "Joining XDP_UMEM addr=[%#lx,%#lx) chunk_size=%u",
                dcache_lo, dcache_hi, umem.chunk_size ));

  if( FD_UNLIKELY(
      0!=setsockopt( xsk_fd, SOL_XDP, XDP_UMEM_REG, &umem, sizeof(struct xdp_umem_reg) ) ) ) {
    FD_LOG_ERR(( "setsockopt(SOL_XDP,XDP_UMEM_REG) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
  }

  FD_LOG_INFO(( "Creating XDP rings (rx_depth=%lu fr_depth=%lu)", rx_depth, fr_depth ));

  FD_TEST( 0==setsockopt( xsk_fd, SOL_XDP, XDP_UMEM_FILL_RING, &fr_depth, sizeof(ulong) ) );
  FD_TEST( 0==setsockopt( xsk_fd, SOL_XDP, XDP_RX_RING,        &rx_depth, sizeof(ulong) ) );

  ulong ring_tx_depth = 64UL;
  ulong ring_cr_depth = 64UL;

  FD_TEST( 0==setsockopt( xsk_fd, SOL_XDP, XDP_TX_RING,              &ring_tx_depth, sizeof(ulong) ) );
  FD_TEST( 0==setsockopt( xsk_fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &ring_cr_depth, sizeof(ulong) ) );

  struct xdp_mmap_offsets offsets = {0};
  socklen_t offsets_sz = sizeof(struct xdp_mmap_offsets);
  FD_TEST( 0==getsockopt( xsk_fd, SOL_XDP, XDP_MMAP_OFFSETS, &offsets, &offsets_sz ) );

  if( poll_mode==FDGEN_XSK_POLL_MODE_BUSY_EXT ||
      poll_mode==FDGEN_XSK_POLL_MODE_BUSY_SYNC ) {

    int prefer_busy_poll = 1;
    FD_TEST( 0==setsockopt( xsk_fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &prefer_busy_poll, sizeof(int)   ) );
    FD_TEST( 0==setsockopt( xsk_fd, SOL_SOCKET, SO_BUSY_POLL,        &busy_poll_usecs,  sizeof(ulong) ) );
    FD_TEST( 0==setsockopt( xsk_fd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &busy_poll_budget, sizeof(ulong) ) );

  }

  double tick_per_ns = fd_tempo_tick_per_ns( NULL );

  fdgen_tile_net_xsk_rx_cfg_t rx_cfg[1] = {{
    .orig        = 1UL,
    .tick_per_ns = tick_per_ns,
    .seq0        = fd_mcache_seq0( mcache ),

    .rng    = rng,
    .cnc    = rx_cnc,
    .mcache = mcache,
    .dcache = dcache,
    .base   = dcache,

    .umem_base = (void *)dcache_lo,
    .frame0    = (void *)dcache_lo,  /* use entire UMEM */
    .mtu       = mtu,
    .xsk_burst = fd_ulong_if( FDGEN_XSK_POLL_MODE_BUSY_SYNC, UINT_MAX, xsk_burst ),
    .lazy      = fd_tempo_lazy_default( fr_depth ),

    .xsk_fd    = xsk_fd,
    .poll_mode = poll_mode
  }};

  fdgen_tile_net_xsk_poll_cfg_t poll_cfg[1] = {{
    .cnc         = poll_cnc,
    .rng         = rng,
    .lazy        = fd_tempo_lazy_default( fr_depth ),
    .tick_per_ns = tick_per_ns,
    .xsk_fd      = xsk_fd,
    .poll_mode   = poll_mode
  }};

  FD_LOG_INFO(( "Joining XDP rings" ));

  rx_cfg->ring_fr.depth  = fr_depth;
  rx_cfg->ring_rx.depth  = rx_depth;

  rx_cfg->ring_fr.map_sz = offsets.fr.desc + rx_cfg->ring_fr.depth * sizeof(ulong);
  rx_cfg->ring_rx.map_sz = offsets.rx.desc + rx_cfg->ring_rx.depth * sizeof(struct xdp_desc);

  rx_cfg->ring_fr.mem    = mmap( NULL, rx_cfg->ring_fr.map_sz, PROT_READ|PROT_WRITE, MAP_SHARED, xsk_fd, XDP_UMEM_PGOFF_FILL_RING );
  rx_cfg->ring_rx.mem    = mmap( NULL, rx_cfg->ring_rx.map_sz, PROT_READ|PROT_WRITE, MAP_SHARED, xsk_fd, XDP_PGOFF_RX_RING        );

  FD_TEST( rx_cfg->ring_fr.mem != MAP_FAILED );
  FD_TEST( rx_cfg->ring_rx.mem != MAP_FAILED );

  rx_cfg->ring_fr.ptr    = rx_cfg->ring_fr.mem + offsets.fr.desc;
  rx_cfg->ring_rx.ptr    = rx_cfg->ring_rx.mem + offsets.rx.desc;

  rx_cfg->ring_fr.flags  = rx_cfg->ring_fr.mem + offsets.fr.flags;
  rx_cfg->ring_rx.flags  = rx_cfg->ring_rx.mem + offsets.rx.flags;

  rx_cfg->ring_fr.prod   = rx_cfg->ring_fr.mem + offsets.fr.producer;
  rx_cfg->ring_rx.prod   = rx_cfg->ring_rx.mem + offsets.rx.producer;

  rx_cfg->ring_fr.cons   = rx_cfg->ring_fr.mem + offsets.fr.consumer;
  rx_cfg->ring_rx.cons   = rx_cfg->ring_rx.mem + offsets.rx.consumer;

  /* Bind XSK to queue on network interface */

  struct sockaddr_xdp sa = {
    .sxdp_family   = PF_XDP,
    .sxdp_ifindex  = if_idx,
    .sxdp_queue_id = 0U,
    .sxdp_flags    = 0
  };
  if( poll_mode==FDGEN_XSK_POLL_MODE_WAKEUP ) {
    sa.sxdp_flags |= XDP_USE_NEED_WAKEUP;
  }

  FD_LOG_INFO(( "Binding to interface %u-%s queue %u", if_idx, iface, sa.sxdp_queue_id ));

  if( FD_UNLIKELY( 0!=bind( xsk_fd, fd_type_pun_const( &sa ), sizeof(struct sockaddr_xdp) ) ) ) {
    FD_LOG_WARNING(( "Unable to bind to interface %u-%s queue %u (%i-%s)",
                     if_idx, iface, sa.sxdp_queue_id, errno, fd_io_strerror( errno ) ));
    return -1;
  }

  /* Register XSK to XDP program */

  FD_LOG_INFO(( "Registering AF_XDP socket with XDP_REDIRECT program" ));

  uint xskmap_key   = 0U;  /* queue */
  int  xskmap_value = xsk_fd;
  FD_TEST( 0==fd_bpf_map_update_elem( redir->xsk_map_fd, &xskmap_key, &xskmap_value, BPF_ANY ) );

  char * rx_tile_argv[1] = { fd_type_pun( rx_cfg ) };
  fd_tile_exec_t * rx_tile = fd_tile_exec_new( 1UL, rx_tile_main, 1, rx_tile_argv );
  FD_TEST( rx_tile );

  if( poll_mode == FDGEN_XSK_POLL_MODE_BUSY_EXT ) {
    char * poll_tile_argv[1] = { fd_type_pun( poll_cfg ) };
    fd_tile_exec_t * poll_tile = fd_tile_exec_new( 2UL, poll_tile_main, 1, poll_tile_argv );
    FD_TEST( poll_tile );
  }

  fdgen_tile_net_xsk_rx_diag_t volatile const * rx_diag = fd_cnc_app_laddr_const( rx_cnc );

  ulong const * seq      = (ulong const *)fd_mcache_seq_laddr_const( mcache );
  ulong         last_seq = fd_mcache_seq_query( seq );
  ulong         dt       = 100e6;
  for(;;) {
    fd_log_sleep( dt );

    ulong cur_seq = fd_mcache_seq_query( seq );
    FD_LOG_NOTICE(( "rate: %10.0f/s", (float)(cur_seq-last_seq)/((float)dt/1e9) ));
    last_seq = cur_seq;

    FD_COMPILER_MFENCE();
    uint fr_cons = rx_diag->fr_cons;
    uint fr_prod = rx_diag->fr_prod;
    uint rx_cons = rx_diag->rx_cons;
    uint rx_prod = rx_diag->rx_prod;
    FD_COMPILER_MFENCE();
    int fr_avail = (int)( fr_prod - fr_cons );
    int rx_avail = (int)( rx_prod - rx_cons );

    FD_LOG_DEBUG(( "fill=%8x/%8x (%8x) rx=%8x/%8x (%8x) in_flight=%5ld",
                    fr_cons, fr_prod, fr_avail,
                    rx_cons, rx_prod, rx_avail,
                    (long)(fr_avail + rx_avail) - (long)fr_depth ));
  }

  /* Clean up */

  fd_wksp_delete_anonymous( wksp );
  fd_rng_delete( fd_rng_leave( rng ) );
  fd_halt();
  return 0;
}
