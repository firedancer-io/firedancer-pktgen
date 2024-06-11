#define _GNU_SOURCE  /* setns(2) */
#include "fdgen_tile_net_xsk_rx.h"
#include "fdgen_tile_net_xsk_poll.h"
#include "../../cfg/fdgen_netlink.h"
#include "../../cfg/fdgen_cfg_net_xdp.h"

/* test_tile_net_xsk_rx.c tests AF_XDP functionality using a veth pair
   in two network namespaces. */

#include <errno.h>          /* errno(3) */
#include <sched.h>          /* setns(2) */
#include <unistd.h>         /* close(2) */
#include <linux/if_xdp.h>   /* xdp_{...} */
#include <linux/netlink.h>  /* NETLINK_ROUTE */
#include <net/if.h>         /* if_nametoindex */
#include <netinet/in.h>     /* sockaddr_in */
#include <sys/mman.h>       /* mmap(2) */

#include <firedancer/tango/cnc/fd_cnc.h>
#include <firedancer/tango/mcache/fd_mcache.h>
#include <firedancer/tango/dcache/fd_dcache.h>
#include <firedancer/tango/tempo/fd_tempo.h>
#include <firedancer/waltz/ebpf/fd_ebpf.h>
#include <firedancer/waltz/xdp/fd_xsk.h>
#include <firedancer/util/net/fd_ip4.h>

static fd_cnc_t *       g_rx_cnc;
static fd_cnc_t *       g_poll_cnc;
static fd_frag_meta_t * g_mcache;
static uchar *          g_dcache;
static ulong            g_mtu;
static int              g_xsk_netns;
static ulong            g_ring_fr_depth;
static ulong            g_ring_rx_depth;

static int
xsk_tile_main( int     argc,
               char ** argv ) {

  fd_rng_t _rng[1]; fd_rng_t * rng = fd_rng_join( fd_rng_new( _rng, fd_tickcount(), 0UL ) );

  fdgen_tile_net_xsk_rx_cfg_t * cfg = fd_type_pun( argv[0] );
  cfg->rng       = rng;
  cfg->lazy      = 100L;
  cfg->xsk_burst = 64UL;

  FD_LOG_NOTICE(( "Starting AF_XDP receive" ));

  int res = fdgen_tile_net_xsk_rx_run( cfg );

  fd_rng_delete( fd_rng_leave( rng ) );
  return res;
}

static int
poll_tile_main( int     argc,
                char ** argv ) {

  fd_rng_t _rng[1]; fd_rng_t * rng = fd_rng_join( fd_rng_new( _rng, fd_tickcount(), 0UL ) );

  FD_TEST( 0==setns( g_xsk_netns, CLONE_NEWNET ) );

  static char const if_name[] = "veth";
  uint if_idx = if_nametoindex( if_name );
  FD_TEST( if_idx );

  FD_LOG_INFO(( "Installing XDP_REDIRECT program" ));

  fdgen_xdp_port_redir_t _redir[1];
  fdgen_xdp_port_redir_t * redir = fdgen_xdp_port_redir_init(
      _redir, 1UL,
      FD_IP4_ADDR( 10, 0, 0, 9 ),
      (fdgen_port_range_t){ 9000, 9100 },
      if_idx,
      0 );
  FD_TEST( redir );

  FD_LOG_INFO(( "Creating AF_XDP socket" ));

  int xsk_fd = socket( AF_XDP, SOCK_RAW, 0 );
  FD_TEST( xsk_fd>=0 );

  ulong dcache_lo = (ulong)g_dcache;
  ulong dcache_hi = (ulong)g_dcache + fd_dcache_data_sz( g_dcache );
        dcache_lo = fd_ulong_align_up( dcache_lo, FD_XSK_UMEM_ALIGN );
        dcache_hi = fd_ulong_align_dn( dcache_hi, FD_XSK_UMEM_ALIGN );
  FD_TEST( dcache_lo < dcache_hi );

  struct xdp_umem_reg umem =
    { .headroom   = 0U,
      .addr       = dcache_lo,
      .chunk_size = g_mtu,
      .len        = dcache_hi - dcache_lo };

  FD_LOG_INFO(( "Joining XDP_UMEM addr=[%#lx,%#lx) chunk_size=%u",
                dcache_lo, dcache_hi, umem.chunk_size ));

  if( FD_UNLIKELY(
      0!=setsockopt( xsk_fd, SOL_XDP, XDP_UMEM_REG, &umem, sizeof(struct xdp_umem_reg) ) ) ) {
    FD_LOG_ERR(( "setsockopt(SOL_XDP,XDP_UMEM_REG) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
  }

  FD_LOG_INFO(( "Creating XDP rings (rx_depth=%lu fr_depth=%lu)",
                g_ring_rx_depth, g_ring_fr_depth ));

  FD_TEST( 0==setsockopt( xsk_fd, SOL_XDP, XDP_UMEM_FILL_RING, &g_ring_fr_depth, sizeof(ulong) ) );
  FD_TEST( 0==setsockopt( xsk_fd, SOL_XDP, XDP_RX_RING,        &g_ring_rx_depth, sizeof(ulong) ) );

  ulong ring_tx_depth = 64UL;
  ulong ring_cr_depth = 64UL;

  FD_TEST( 0==setsockopt( xsk_fd, SOL_XDP, XDP_TX_RING,              &ring_tx_depth, sizeof(ulong) ) );
  FD_TEST( 0==setsockopt( xsk_fd, SOL_XDP, XDP_UMEM_COMPLETION_RING, &ring_cr_depth, sizeof(ulong) ) );

  struct xdp_mmap_offsets offsets = {0};
  socklen_t offsets_sz = sizeof(struct xdp_mmap_offsets);
  FD_TEST( 0==getsockopt( xsk_fd, SOL_XDP, XDP_MMAP_OFFSETS, &offsets, &offsets_sz ) );

  double tick_per_ns = fd_tempo_tick_per_ns( NULL );

  fdgen_tile_net_xsk_rx_cfg_t rx_cfg[1] = {{
    .orig        = 1UL,
    .tick_per_ns = tick_per_ns,
    .seq0        = fd_mcache_seq0( g_mcache ),

    .cnc    = g_rx_cnc,
    .mcache = g_mcache,
    .dcache = g_dcache,
    .base   = g_dcache,

    .umem_base =  g_dcache
  }};

  FD_LOG_INFO(( "Joining XDP rings" ));

  rx_cfg->ring_fr.depth  = g_ring_fr_depth;
  rx_cfg->ring_rx.depth  = g_ring_rx_depth;

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
    .sxdp_flags    = XDP_USE_NEED_WAKEUP
  };

  FD_LOG_INFO(( "Binding to interface %u-%s queue %u", if_idx, if_name, sa.sxdp_queue_id ));

  if( FD_UNLIKELY( 0!=bind( xsk_fd, fd_type_pun_const( &sa ), sizeof(struct sockaddr_xdp) ) ) ) {
    FD_LOG_WARNING(( "Unable to bind to interface %u-%s queue %u (%i-%s)",
                     if_idx, if_name, sa.sxdp_queue_id, errno, fd_io_strerror( errno ) ));
    return -1;
  }

  /* Register XSK to XDP program */

  FD_LOG_INFO(( "Registering AF_XDP socket with XDP_REDIRECT program" ));

  uint xskmap_key   = 0U;  /* queue */
  int  xskmap_value = xsk_fd;
  FD_TEST( 0==fd_bpf_map_update_elem( redir->xsk_map_fd, &xskmap_key, &xskmap_value, BPF_ANY ) );

  /* Run */

  char * rx_tile_argv[1] = { fd_type_pun( rx_cfg ) };
  fd_tile_exec_t * rx_tile = fd_tile_exec_new( 2UL, xsk_tile_main, 1, rx_tile_argv );

  fdgen_tile_net_xsk_poll_cfg_t poll_cfg[1] = {{
    .cnc         = g_poll_cnc,
    .rng         = rng,
    .lazy        = 100L,
    .tick_per_ns = tick_per_ns,
    .xsk_fd      = xsk_fd
  }};

  fdgen_tile_net_xsk_poll_run( poll_cfg );
  FD_LOG_ERR(( "poll end" ));

  fd_tile_exec_delete( rx_tile, NULL );
  close( xsk_fd );
  fd_rng_delete( fd_rng_leave( rng ) );
  fdgen_xdp_port_redir_fini( redir );
  return 0;
}

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  ulong cpu_idx = fd_tile_cpu_id( fd_tile_idx() );
  if( cpu_idx>=fd_shmem_cpu_cnt() ) cpu_idx = 0UL;

  char const * _page_sz     = fd_env_strip_cmdline_cstr ( &argc, &argv, "--page-sz",      NULL, "gigantic"                 );
  ulong        page_cnt     = fd_env_strip_cmdline_ulong( &argc, &argv, "--page-cnt",     NULL, 1UL                        );
  ulong        numa_idx     = fd_env_strip_cmdline_ulong( &argc, &argv, "--numa-idx",     NULL, fd_shmem_numa_idx(cpu_idx) );
  ulong        depth        = fd_env_strip_cmdline_ulong( &argc, &argv, "--depth",        NULL, 1024UL                     );
  ulong        mtu          = fd_env_strip_cmdline_ulong( &argc, &argv, "--mtu",          NULL, 2048UL                     );
  ulong        xsk_rx_depth = fd_env_strip_cmdline_ulong( &argc, &argv, "--xsk-rx-depth", NULL, 1024UL                     );
  ulong        xsk_fr_depth = fd_env_strip_cmdline_ulong( &argc, &argv, "--xsk-fr-depth", NULL, 1024UL                     );

  g_mtu           = mtu;
  g_ring_fr_depth = xsk_fr_depth;
  g_ring_rx_depth = xsk_rx_depth;

  ulong page_sz = fd_cstr_to_shmem_page_sz( _page_sz );
  if( FD_UNLIKELY( !page_sz ) ) FD_LOG_ERR(( "unsupported --page-sz" ));

  if( FD_UNLIKELY( fd_tile_cnt()<3 ) ) FD_LOG_ERR(( "This test requires at least 3 tiles" ));

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
  g_xsk_netns = veth_env->params[1].netns;

  /* Allocate objects */

  void *     rx_cnc_mem = fd_wksp_alloc_laddr( wksp, fd_cnc_align(), fd_cnc_footprint( 64UL ), 1UL );
  fd_cnc_t * rx_cnc     = fd_cnc_join( fd_cnc_new( rx_cnc_mem, 64UL, 1UL, fd_tickcount() ) );
  FD_TEST( rx_cnc );
  g_rx_cnc = rx_cnc;

  void *     poll_cnc_mem = fd_wksp_alloc_laddr( wksp, fd_cnc_align(), fd_cnc_footprint( 64UL ), 1UL );
  fd_cnc_t * poll_cnc     = fd_cnc_join( fd_cnc_new( poll_cnc_mem, 64UL, 1UL, fd_tickcount() ) );
  FD_TEST( poll_cnc );
  g_poll_cnc = poll_cnc;

  if( FD_UNLIKELY( !fd_mcache_footprint( depth, 0UL ) ) ) FD_LOG_ERR(( "invalid depth" ));
  ulong            seq0       = 0UL;
  void *           mcache_mem = fd_wksp_alloc_laddr( wksp, fd_mcache_align(), fd_mcache_footprint( depth, 0UL ), 1UL );
  fd_frag_meta_t * mcache     = fd_mcache_join( fd_mcache_new( mcache_mem, depth, 0UL, seq0 ) );
  FD_TEST( mcache );
  g_mcache = mcache;

  if( FD_UNLIKELY( mtu!=2048 && mtu!=4096 ) ) FD_LOG_ERR(( "invalid mtu" ));
  ulong   dcache_data_sz = mtu * fd_ulong_max( g_ring_rx_depth, g_ring_fr_depth );
  void *  dcache_mem     = fd_wksp_alloc_laddr( wksp, FD_DCACHE_ALIGN, fd_dcache_footprint( dcache_data_sz, 0UL ) + FD_XSK_UMEM_ALIGN, 1UL );
  uchar * dcache         = fd_dcache_join( fd_dcache_new( dcache_mem, dcache_data_sz, 0UL ) );
  FD_TEST( dcache );
  g_dcache = dcache;

  /* Spawn tiles */

  fd_tile_exec_t * poll_tile = fd_tile_exec_new( 1UL, poll_tile_main, 0, NULL );
  FD_TEST( poll_tile );

  FD_TEST( 0==setns( veth_env->params[0].netns, CLONE_NEWNET ) );

  /* Send packets */

  int udp_sock = socket( AF_INET, SOCK_DGRAM, 0 );
  FD_TEST( udp_sock>=0 );

  struct sockaddr_in sock_dst = {
    .sin_family      = AF_INET,
    .sin_port        = (ushort)fd_ushort_bswap( 9000 ),
    .sin_addr.s_addr = FD_IP4_ADDR( 10, 0, 0, 9 )
  };

  for(;;) {
    sendto( udp_sock, "hello", 5UL, 0, fd_type_pun_const( &sock_dst ), sizeof(struct sockaddr_in) );
  }

  FD_LOG_INFO(( "Cleaning up" ));

  close( udp_sock );

  fd_tile_exec_delete( poll_tile, NULL );

  fd_wksp_free_laddr( fd_dcache_delete( fd_dcache_leave( dcache   ) ) );
  fd_wksp_free_laddr( fd_mcache_delete( fd_mcache_leave( mcache   ) ) );
  fd_wksp_free_laddr( fd_cnc_delete   ( fd_cnc_leave   ( rx_cnc   ) ) );
  fd_wksp_free_laddr( fd_cnc_delete   ( fd_cnc_leave   ( poll_cnc ) ) );

  close( veth_env->params[0].netns );
  close( veth_env->params[1].netns );

  fd_wksp_delete_anonymous( wksp );

  FD_LOG_NOTICE(( "pass" ));
  fd_halt();
  return 0;
}
