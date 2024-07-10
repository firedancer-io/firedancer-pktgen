#define _GNU_SOURCE
#include "fdgen_tile_net_dgram_rxtx.h"
#include "fdgen_tile_net_dgram.h"
#include <firedancer/tango/cnc/fd_cnc.h>
#include <firedancer/tango/mcache/fd_mcache.h>
#include <firedancer/tango/dcache/fd_dcache.h>
#include <firedancer/tango/tempo/fd_tempo.h>
#include <firedancer/util/net/fd_eth.h>
#include <firedancer/util/net/fd_ip4.h>
#include <firedancer/util/net/fd_udp.h>

#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* Topology:

    ┌────┐     ┌────┐
    │recv◄─────┤    ◄────┐
    └────┘     │    │    │
        tango  │rxtx│    │
    ┌────┐     │    │ lo │
    │send├─────►    ├────┘
    └────┘     └────┘

*/

/* Tile 1: send (tango) ***********************************************/

struct test_send_args {
  fd_wksp_t *      wksp;
  fd_cnc_t *       cnc;
  fd_frag_meta_t * mcache;
  uchar *          dcache;

  uint   seed;
  long   lazy;
  ulong  mtu;
  uint   dst_ip;   /* net order */
  ushort dst_port; /* host order */
};

typedef struct test_send_args test_send_args_t;

static int
send_tile_main( int     argc,
                char ** argv ) {

  assert( argc==1 );
  test_send_args_t * args = fd_type_pun( argv[0] );

  void * base     = (void *)args->wksp;
  ulong  orig     = fd_tile_idx();
  uint   dst_ip   = args->dst_ip;
  ushort dst_port = args->dst_port;

  /* Hook up to tx cnc */
  fd_cnc_t * cnc = args->cnc;

  /* Hook up to tx mcache */
  fd_frag_meta_t * mcache = args->mcache;
  ulong            depth  = fd_mcache_depth( mcache );
  ulong *          sync   = fd_mcache_seq_laddr( mcache );
  ulong            seq    = fd_mcache_seq_query( sync );

  /* Hook up to tx dcache */
  uchar * dcache = args->dcache;
  ulong   chunk0 = fd_dcache_compact_chunk0( base, dcache );
  ulong   wmark  = fd_dcache_compact_wmark ( base, dcache, args->mtu );
  ulong   chunk  = chunk0;

  /* Hook up to the random number generator */
  uint seed = (uint)( args->seed + fd_tile_idx() );
  fd_rng_t _rng[1];
  fd_rng_t * rng = fd_rng_join( fd_rng_new( _rng, seed, 0UL ) );

  /* Configure housekeeping */
  float tick_per_ns = (float)fd_tempo_tick_per_ns( NULL );
  ulong async_min = fd_tempo_async_min( args->lazy, 1UL /*event_cnt*/, tick_per_ns );
  if( FD_UNLIKELY( !async_min ) ) FD_LOG_ERR(( "bad lazy" ));

  fd_cnc_signal( cnc, FD_CNC_SIGNAL_RUN );
  long now  = fd_tickcount();
  long then = now;
  for(;;) {
    if( FD_UNLIKELY( (now-then)>=0L ) ) {
      fd_mcache_seq_update( sync, seq );
      fd_cnc_heartbeat( cnc, now );
      ulong s = fd_cnc_signal_query( cnc );
      if( FD_UNLIKELY( s!=FD_CNC_SIGNAL_RUN ) ) {
        if( FD_UNLIKELY( s!=FD_CNC_SIGNAL_HALT ) ) FD_LOG_ERR(( "Unexpected signal" ));
        break;
      }
      then = now + (long)fd_tempo_async_reload( rng, async_min );
    }

    uchar *        pkt     = fd_chunk_to_laddr( base, chunk );
    fd_eth_hdr_t * eth_hdr = fd_type_pun( pkt    );
    fd_ip4_hdr_t * ip4_hdr = fd_type_pun( pkt+14 );
    fd_udp_hdr_t * udp_hdr = fd_type_pun( pkt+34 );
    eth_hdr->net_type = FD_ETH_HDR_TYPE_IP;
    ip4_hdr[0] = (fd_ip4_hdr_t) {
      .verihl       = FD_IP4_VERIHL( 4, 5 ),
      .net_tot_len  = (ushort)fd_ushort_bswap( 64 ),
      .net_frag_off = (ushort)fd_ushort_bswap( FD_IP4_HDR_FRAG_OFF_DF ),
      .ttl          = 1,
      .protocol     = FD_IP4_HDR_PROTOCOL_UDP
    };
    memcpy( ip4_hdr->daddr_c, &dst_ip, 4 );
    udp_hdr[0] = (fd_udp_hdr_t) {
      .net_sport = (ushort)fd_ushort_bswap( 0x1234 ),
      .net_dport = (ushort)fd_ushort_bswap( (ushort)dst_port ),
      .net_len   = (ushort)fd_ushort_bswap( 8 ),
      .check     = 0
    };

    ulong sz     = 42UL;
    ulong ctl    = fd_frag_meta_ctl( orig, 1, 1, 0 );
    ulong sig    = 0UL;
    ulong tsorig = 0UL;
    ulong tspub  = 0UL;
    fd_mcache_publish( mcache, depth, seq, sig, chunk, sz, ctl, tsorig, tspub );

    chunk = fd_dcache_compact_next( chunk, sz, chunk0, wmark );
    seq   = fd_seq_inc( seq, 1UL );
    now   = fd_tickcount();
  }

  fd_cnc_signal( cnc, FD_CNC_SIGNAL_BOOT );
  return 0;
}

/* Tile 2: recv (socket) **********************************************/

struct test_recv_args {
  fd_wksp_t *      wksp;
  fd_cnc_t *       cnc;
  fd_frag_meta_t * mcache;

  long lazy;
  uint seed;
};

typedef struct test_recv_args test_recv_args_t;

static int
recv_tile_main( int     argc,
                char ** argv ) {

  assert( argc==1 );
  test_recv_args_t * args = fd_type_pun( argv[0] );
  void * base = (void *)args->wksp;

  /* Hook up to rx cnc */
  fd_cnc_t * cnc = args->cnc;

  /* Hook up to rx mcache */
  fd_frag_meta_t * mcache = args->mcache;
  ulong            depth  = fd_mcache_depth( mcache );
  ulong *          sync   = fd_mcache_seq_laddr( mcache );
  ulong            seq    = fd_mcache_seq_query( sync );

  /* Hook up to the random number generator */
  uint seed = (uint)( args->seed + fd_tile_idx() );
  fd_rng_t _rng[1];
  fd_rng_t * rng = fd_rng_join( fd_rng_new( _rng, seed, 0UL ) );

  ulong ovrnp_cnt = 0UL; /* Count of overruns while polling for next seq */
  ulong ovrnr_cnt = 0UL; /* Count of overruns while processing seq payload */

  float tick_per_ns = (float)fd_tempo_tick_per_ns( NULL );
  ulong async_min = fd_tempo_async_min( args->lazy, 1UL /*event_cnt*/, tick_per_ns );
  if( FD_UNLIKELY( !async_min ) ) FD_LOG_ERR(( "bad lazy" ));
  ulong async_rem = 1UL; /* Do housekeeping on first iteration */

  long  then = fd_log_wallclock();
  ulong iter = 0UL;

  fd_cnc_signal( cnc, FD_CNC_SIGNAL_RUN );
  for(;;) {

    fd_frag_meta_t const * mline;
    ulong                  seq_found;
    long                   diff;

    ulong sig;
    ulong chunk;
    ulong sz;
    ulong ctl;
    ulong tsorig;
    ulong tspub;
    FD_MCACHE_WAIT_REG( sig, chunk, sz, ctl, tsorig, tspub, mline, seq_found, diff, async_rem, mcache, depth, seq );

    /* FIXME look at message data */
    (void)tspub; (void)tsorig; (void)ctl; (void)sz; (void)chunk; (void)sig; (void)base;

    if( FD_UNLIKELY( !async_rem ) ) {
      long now = fd_log_wallclock();
      fd_cnc_heartbeat( cnc, now );

      long dt = now - then;
      if( FD_UNLIKELY( dt > (long)1e9 ) ) {
        float mfps = (1e3f*(float)iter) / (float)dt;
        FD_LOG_NOTICE(( "%7.3f Mfrag/s rx (ovrnp %lu ovrnr %lu)", (double)mfps, ovrnp_cnt, ovrnr_cnt ));
        ovrnp_cnt = 0UL;
        ovrnr_cnt = 0UL;
        then      = now;
        iter      = 0UL;
      }

      ulong s = fd_cnc_signal_query( cnc );
      if( FD_UNLIKELY( s!=FD_CNC_SIGNAL_RUN ) ) {
        if( FD_UNLIKELY( s!=FD_CNC_SIGNAL_HALT ) ) FD_LOG_ERR(( "Unexpected signal" ));
        break;
      }

      async_rem = fd_tempo_async_reload( rng, async_min );
      continue;
    }

    if( FD_UNLIKELY( diff ) ) {
      ovrnp_cnt++;
      seq = seq_found;
      continue;
    }

    seq_found = fd_frag_meta_seq_query( mline );
    if( FD_UNLIKELY( fd_seq_ne( seq_found, seq ) ) ) {
      ovrnr_cnt++;
      seq = seq_found;
      continue;
    }

    seq = fd_seq_inc( seq, 1UL );
    iter++;
  }

  fd_cnc_signal( cnc, FD_CNC_SIGNAL_BOOT );
  fd_rng_delete( fd_rng_leave( rng ) );
  return 0;
}

/* Tile 3: rxtx *******************************************************/

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
  ulong so_rcvbuf;
  ulong so_sndbuf;

  uint   bind_ip;   /* net order */
  ushort bind_port; /* host order */
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
  if( FD_UNLIKELY( epoll_fd<0 ) ) {
    FD_LOG_WARNING(( "epoll_create1 failed (%i-%s)", errno, fd_io_strerror( errno ) ));
    return 1;
  }

  int listen_fd = socket( AF_INET, SOCK_DGRAM, 0 );
  if( FD_UNLIKELY( listen_fd<0 ) ) {
    FD_LOG_WARNING(( "socket(AF_INET,SOCK_DGRAM,0) failed (%i-%s)", errno, fd_io_strerror( errno ) ));
    return 1;
  }
  if( FD_UNLIKELY( 0!=setsockopt( listen_fd, SOL_SOCKET, SO_RCVBUF, &args->so_rcvbuf, sizeof(ulong) ) ) ) {
    FD_LOG_WARNING(( "setsockopt(SO_RCVBUF) failed (%i-%s)", errno, fd_io_strerror( errno ) ));
    return 1;
  }
  struct sockaddr_in listen_addr = {
    .sin_family = AF_INET,
    .sin_addr   = { .s_addr = args->bind_ip },
    .sin_port   = (ushort)fd_ushort_bswap( (ushort)args->bind_port )
  };
  if( FD_UNLIKELY( 0!=bind( listen_fd, fd_type_pun( &listen_addr ), sizeof(struct sockaddr_in) ) ) ) {
    FD_LOG_WARNING(( "bind(" FD_IP4_ADDR_FMT ":%u) failed (%i-%s)", FD_IP4_ADDR_FMT_ARGS( args->bind_ip ), 9090, errno, fd_io_strerror( errno ) ));
    return 1;
  }
  fdgen_tile_net_dgram_epoll_data_t epoll_data = { .fd = listen_fd, .dport = args->bind_port };
  struct epoll_event epoll_ev = {
    .events = EPOLLIN,
    .data   = { .u64 = epoll_data.u64 }
  };
  if( FD_UNLIKELY( 0!=epoll_ctl( epoll_fd, EPOLL_CTL_ADD, listen_fd, &epoll_ev ) ) ) {
    FD_LOG_WARNING(( "epoll_ctl(EPOLL_CTL_ADD) failed (%i-%s)", errno, fd_io_strerror( errno ) ));
    return 1;
  }

  int send_fd = socket( AF_INET, SOCK_DGRAM, 0 );
  if( FD_UNLIKELY( send_fd<0 ) ) {
    FD_LOG_WARNING(( "socket(AF_INET,SOCK_DGRAM,0) failed (%i-%s)", errno, fd_io_strerror( errno ) ));
    return 1;
  }
  if( FD_UNLIKELY( 0!=setsockopt( listen_fd, SOL_SOCKET, SO_SNDBUF, &args->so_sndbuf, sizeof(ulong) ) ) ) {
    FD_LOG_WARNING(( "setsockopt(SO_SNDBUF) failed (%i-%s)", errno, fd_io_strerror( errno ) ));
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
    .send_fd  = send_fd,

    .scratch    = scratch,
    .scratch_sz = scratch_sz,
  }};

  int res = fdgen_tile_net_dgram_rxtx_run( cfg );

  close( epoll_fd  );
  close( send_fd   );
  close( listen_fd );
  return res;
}

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  ulong cpu_idx = fd_tile_cpu_id( fd_tile_idx() );
  if( cpu_idx>=fd_shmem_cpu_cnt() ) cpu_idx = 0UL;

  char const * _page_sz  = fd_env_strip_cmdline_cstr ( &argc, &argv, "--page-sz",      NULL, "gigantic"                 );
  ulong        page_cnt  = fd_env_strip_cmdline_ulong( &argc, &argv, "--page-cnt",     NULL, 1UL                        );
  ulong        numa_idx  = fd_env_strip_cmdline_ulong( &argc, &argv, "--numa-idx",     NULL, fd_shmem_numa_idx(cpu_idx) );
  ulong        rx_depth  = fd_env_strip_cmdline_ulong( &argc, &argv, "--rx-depth",     NULL, 1024UL                     );
  ulong        tx_depth  = fd_env_strip_cmdline_ulong( &argc, &argv, "--tx-depth",     NULL, 1024UL                     );
  ulong        rx_burst  = fd_env_strip_cmdline_ulong( &argc, &argv, "--rx-burst",     NULL,  128UL                     );
  ulong        tx_burst  = fd_env_strip_cmdline_ulong( &argc, &argv, "--tx-burst",     NULL,  128UL                     );
  ulong        so_rcvbuf = fd_env_strip_cmdline_ulong( &argc, &argv, "--so-rcvbuf",    NULL, 1UL<<17                    );
  ulong        so_sndbuf = fd_env_strip_cmdline_ulong( &argc, &argv, "--so-sndbuf",    NULL, 1UL<<17                    );
  ulong        mtu       = fd_env_strip_cmdline_ulong( &argc, &argv, "--mtu",          NULL, 1500UL                     );
  uint         seed      = fd_env_strip_cmdline_uint ( &argc, &argv, "--seed",         NULL,    0U                      );

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

  ulong   tx_dcache_data_sz = fd_dcache_req_data_sz( mtu, tx_depth, 1UL, 1 );
  void *  tx_dcache_mem     = fd_wksp_alloc_laddr( wksp, fd_dcache_align(), fd_dcache_footprint( tx_dcache_data_sz, 0UL ), 1UL );
  uchar * tx_dcache         = fd_dcache_join( fd_dcache_new( tx_dcache_mem, tx_dcache_data_sz, 0UL ) );
  FD_TEST( tx_dcache );

  /* Spawn tiles */

  test_send_args_t send_args = {
    .wksp     = wksp,
    .cnc      = send_cnc,
    .mcache   = tx_mcache,
    .dcache   = tx_dcache,
    .seed     = seed,
    .lazy     = 1e6, /* 1ms is sufficient for housekeeping */
    .mtu      = mtu,

    .dst_ip   = FD_IP4_ADDR( 127, 0, 0, 1 ),
    .dst_port = 9090,
  };
  char * send_tile_argv[1] = { fd_type_pun( &send_args ) };

  test_recv_args_t recv_args = {
    .wksp   = wksp,
    .cnc    = recv_cnc,
    .mcache = rx_mcache,
    .lazy   = 1e6, /* 1ms is sufficient for housekeeping */
    .seed   = seed,
  };
  char * recv_tile_argv[1] = { fd_type_pun( &recv_args ) };

  test_rxtx_args_t rxtx_args = {
    .wksp    = wksp,
    .cnc     = rxtx_cnc,
    .lazy    = 100,  /* TODO */
    .mtu     = mtu,
    .seed    = seed,

    .tx_mcache = tx_mcache,
    .tx_burst  = tx_burst,
    .rx_mcache = rx_mcache,
    .rx_dcache = rx_dcache,
    .rx_burst  = rx_burst,
    .so_rcvbuf = so_rcvbuf,
    .so_sndbuf = so_sndbuf,

    .bind_ip   = send_args.dst_ip,
    .bind_port = send_args.dst_port
  };
  char * rxtx_tile_argv[1] = { fd_type_pun( &rxtx_args ) };

  fd_tile_exec_t * send_tile = fd_tile_exec_new( 1UL, send_tile_main, 1, send_tile_argv );
  fd_tile_exec_t * recv_tile = fd_tile_exec_new( 2UL, recv_tile_main, 1, recv_tile_argv );
  fd_tile_exec_t * rxtx_tile = fd_tile_exec_new( 3UL, rxtx_tile_main, 1, rxtx_tile_argv );

  FD_TEST( fd_cnc_wait( send_cnc, FD_CNC_SIGNAL_BOOT, (long)5e9, NULL )==FD_CNC_SIGNAL_RUN );
  FD_TEST( fd_cnc_wait( recv_cnc, FD_CNC_SIGNAL_BOOT, (long)5e9, NULL )==FD_CNC_SIGNAL_RUN );
  FD_TEST( fd_cnc_wait( rxtx_cnc, FD_CNC_SIGNAL_BOOT, (long)5e9, NULL )==FD_CNC_SIGNAL_RUN );

  sleep( 10 );

  FD_LOG_INFO(( "Cleaning up" ));

  FD_TEST( !fd_cnc_open( rxtx_cnc  ) );
  FD_TEST( !fd_cnc_open( send_cnc ) );
  FD_TEST( !fd_cnc_open( recv_cnc ) );

  fd_cnc_signal( send_cnc, FD_CNC_SIGNAL_HALT );
  fd_cnc_signal( recv_cnc, FD_CNC_SIGNAL_HALT );
  fd_cnc_signal( rxtx_cnc, FD_CNC_SIGNAL_HALT );

  fd_cnc_close( send_cnc );
  fd_cnc_close( recv_cnc );
  fd_cnc_close( rxtx_cnc );

  FD_TEST( fd_cnc_wait( send_cnc, FD_CNC_SIGNAL_HALT, (long)5e9, NULL )==FD_CNC_SIGNAL_BOOT );
  FD_TEST( fd_cnc_wait( recv_cnc, FD_CNC_SIGNAL_HALT, (long)5e9, NULL )==FD_CNC_SIGNAL_BOOT );
  FD_TEST( fd_cnc_wait( rxtx_cnc, FD_CNC_SIGNAL_HALT, (long)5e9, NULL )==FD_CNC_SIGNAL_BOOT );

  fd_tile_exec_delete( recv_tile, NULL );
  fd_tile_exec_delete( rxtx_tile, NULL );
  fd_tile_exec_delete( send_tile, NULL );

  fd_wksp_free_laddr( fd_cnc_delete( fd_cnc_leave( rxtx_cnc ) ) );
  fd_wksp_free_laddr( fd_cnc_delete( fd_cnc_leave( recv_cnc ) ) );
  fd_wksp_free_laddr( fd_cnc_delete( fd_cnc_leave( send_cnc ) ) );

  fd_wksp_delete_anonymous( wksp );

  FD_LOG_NOTICE(( "pass" ));
  fd_halt();
  return 0;
}

