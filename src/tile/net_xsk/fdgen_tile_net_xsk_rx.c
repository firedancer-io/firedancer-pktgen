#include "fdgen_tile_net_xsk.h"
#include "fdgen_tile_net_xsk_rx.h"

#include <assert.h>
#include <errno.h>
#include <linux/if_xdp.h>
#include <poll.h>

#include <firedancer/tango/fd_tango_base.h>
#include <firedancer/tango/cnc/fd_cnc.h>
#include <firedancer/tango/mcache/fd_mcache.h>
#include <firedancer/tango/dcache/fd_dcache.h>
#include <firedancer/tango/tempo/fd_tempo.h>
#include <firedancer/waltz/xdp/fd_xsk.h>

/* init_rings sets up the initial allocation of frames between mcache
   and xsk fill ring.  This is essential as the message queues also
   serve as FIFO allocators.

   mcache:    ring that will own initial 'published' frames.
   fill:      ring that will own initial 'free' frames.
   chunk0:    base address of the fd_tango MPMC session that mcache belongs to
   umem_base: base address of the AF_XDP UMEM region
   mtu:       AF_XDP frame size
   frame0:    pointer to frame in dcache with lowest address */

static void
init_rings( fd_frag_meta_t *   mcache,
            fdgen_xsk_ring_t * fill,
            uchar *            chunk0,
            uchar *            umem_base,
            uchar *            frame0,
            ulong              mtu ) {

  /* Assign frames to mcache (2^n) */

  uchar * frame = frame0;
  ulong mcache_depth = fd_mcache_depth( mcache );

  for( ulong j=0UL; j<mcache_depth; j++ ) {
    mcache[j].ctl    = fd_frag_meta_ctl( 0, 0, 0, /* err */ 1 );
    mcache[j].chunk  = fd_laddr_to_chunk( chunk0, frame );
    frame           += mtu;
  }

  /* Assign frames to fill ring (2^n - 1) */

  ulong * fill_ring  = fill->frame_ring;
  ulong   fill_depth = fill->depth - 1UL;

  for( ulong j=0UL; j<fill_depth; j++ ) {
    fill_ring[j]  = fd_laddr_to_umem( umem_base, frame );;
    frame        += mtu;
  }

  FD_COMPILER_MFENCE();
  FD_VOLATILE( fill->prod[0] ) = (uint)fill_depth;
  FD_COMPILER_MFENCE();

}

static void
xsk_poll_recv( int xsk_fd ) {
  struct msghdr _ignored[ 1 ] = { 0 };
  if( FD_UNLIKELY( -1==recvmsg( xsk_fd, _ignored, MSG_DONTWAIT ) ) ) {
    if( FD_UNLIKELY( errno!=EAGAIN ) ) {
      FD_LOG_WARNING(( "xsk recvmsg failed (%i-%s)", errno, fd_io_strerror( errno ) ));
    }
  }
}

int
fdgen_tile_net_xsk_rx_run( fdgen_tile_net_xsk_rx_cfg_t * cfg ) {

  if( FD_UNLIKELY( !cfg ) ) { FD_LOG_WARNING(( "NULL cfg" )); return 1; }

  /* load config */

  fd_cnc_t *       cnc         = cfg->cnc;
  ulong            orig        = cfg->orig;
  fd_rng_t *       rng         = cfg->rng;
  fd_frag_meta_t * mcache      = cfg->mcache;
  uchar *          dcache      = cfg->dcache;
  uchar *          base        = cfg->base;
  long             lazy        = cfg->lazy;
  double           tick_per_ns = cfg->tick_per_ns;
  fdgen_xsk_ring_t fill        = cfg->ring_fr;
  fdgen_xsk_ring_t rx          = cfg->ring_rx;
  void *           umem_base   = cfg->umem_base;
  void *           frame0      = cfg->frame0;
  ulong            xsk_burst   = cfg->xsk_burst;
  ulong            mtu         = cfg->mtu;
  ulong            total_depth;

  /* cnc state */
  fdgen_tile_net_xsk_rx_diag_t * cnc_diag;
  ulong   cnc_diag_in_backp;      /* is the run loop currently backpressured by fill ring, in [0,1] */
  ulong   cnc_diag_backp_cnt;     /* Accumulates number of transitions of tile to backpressured between housekeeping events */
  ulong   cnc_diag_pcap_pub_cnt;  /* Accumulates number of XDP frags published between housekeeping events */
  ulong   cnc_diag_pcap_pub_sz;   /* Accumulates XDP payload bytes publised between housekeeping events */

  /* out frag stream state */
  ulong   mcache_depth; /* ==fd_mcache_depth( mcache ), depth of the mcache / positive integer power of 2 */
  ulong * sync;         /* ==fd_mcache_seq_laddr( mcache ), local addr where mcache sync info is published */
  ulong   seq;          /* frag sequence number to publish */

  /* housekeeping state */
  ulong async_min; /* minimum number of ticks between processing a housekeeping event, positive integer power of 2 */

  /* XSK queue pointers */
  uint       volatile * fill_prod_p;
  uint const volatile * fill_cons_p;
  uint const volatile * rx_prod_p;
  uint       volatile * rx_cons_p;

  /* cached XSK queue states */
  uint   fill_prod;  /* owned */
  uint   fill_cons;  /* stale */
  uint   rx_prod;    /* stale */
  uint   rx_cons;    /* owned */
  ulong  seq_flush;  /* flush dirty & prefetch read cache at this seq */

# define XSK_SYNC()                  \
  do {                               \
    FD_COMPILER_MFENCE();            \
    FD_VOLATILE( rx_cons_p  [0] ) = rx_cons;        \
    FD_COMPILER_MFENCE();            \
    FD_VOLATILE( fill_prod_p[0] ) = fill_prod;      \
    fill_cons      = FD_VOLATILE_CONST( fill_cons_p[0] ); \
    rx_prod        = FD_VOLATILE_CONST( rx_prod_p  [0] ); \
    FD_COMPILER_MFENCE();            \
  } while(0)

  do {

    FD_LOG_INFO(( "Booting net_xsk_rx" ));

    /* cnc state init */

    if( FD_UNLIKELY( !cnc ) ) { FD_LOG_WARNING(( "NULL cnc" )); return 1; }
    if( FD_UNLIKELY( fd_cnc_app_sz( cnc )<sizeof(fdgen_tile_net_xsk_rx_diag_t) ) ) { FD_LOG_WARNING(( "undersz cnc diag" )); return 1; }
    if( FD_UNLIKELY( fd_cnc_signal_query( cnc )!=FD_CNC_SIGNAL_BOOT ) ) { FD_LOG_WARNING(( "already booted" )); return 1; }

    cnc_diag = fd_cnc_app_laddr( cnc );

    cnc_diag_in_backp      = 1UL;
    cnc_diag_backp_cnt     = 0UL;
    cnc_diag_pcap_pub_cnt  = 0UL;
    cnc_diag_pcap_pub_sz   = 0UL;

    /* out frag stream init */

    if( FD_UNLIKELY( !mcache ) ) { FD_LOG_WARNING(( "NULL mcache" )); return 1; }
    mcache_depth = fd_mcache_depth    ( mcache );
    sync         = fd_mcache_seq_laddr( mcache );

    seq = fd_mcache_seq_query( sync );

    if( FD_UNLIKELY( !dcache ) ) { FD_LOG_WARNING(( "NULL dcache" )); return 1; }
    if( FD_UNLIKELY( !base   ) ) { FD_LOG_WARNING(( "NULL base"   )); return 1; }

    /* check buffer space */

    if( FD_UNLIKELY( __builtin_uaddl_overflow( mcache_depth, cfg->ring_fr.depth, &total_depth ) ) ) {
      FD_LOG_WARNING(( "mcache+fill depth overflow" ));
      return 1;
    }

    if( FD_UNLIKELY( !mtu || !fd_ulong_is_aligned( mtu, 2048UL ) ) ) {
      FD_LOG_WARNING(( "invalid MTU" ));
      return 1;
    }

    ulong total_bufsz;
    if( FD_UNLIKELY( __builtin_umull_overflow( total_depth, mtu, &total_bufsz ) ) ) {
      FD_LOG_WARNING(( "mcache+fill depth overflow" ));
      return 1;
    }

    ulong dcache_data_sz = fd_dcache_data_sz( dcache );
    if( FD_UNLIKELY( dcache_data_sz < total_bufsz ) ) {
      FD_LOG_WARNING(( "dcache data sz too small (have %#lx, need %#lx)", dcache_data_sz, total_bufsz ));
      return 1;
    }

    /* mcache init */

    if( FD_UNLIKELY( !cfg->ring_fr.ptr ) ) {
      FD_LOG_WARNING(( "NULL fill ring ptr" ));
      return 1;
    }

    if( FD_UNLIKELY( !cfg->ring_rx.ptr ) ) {
      FD_LOG_WARNING(( "NULL rx ring ptr" ));
      return 1;
    }

    if( FD_UNLIKELY( !umem_base || !fd_ulong_is_aligned( (ulong)umem_base, FD_XSK_UMEM_ALIGN ) ) ) {
      FD_LOG_WARNING(( "invalid UMEM base address" ));
      return 1;
    }

    if( FD_UNLIKELY( frame0 < umem_base || !fd_ulong_is_aligned( (ulong)frame0, FD_CHUNK_ALIGN ) ) ) {
      FD_LOG_WARNING(( "invalid frame0 address" ));
      return 1;
    }

    init_rings( mcache, &cfg->ring_fr, base, umem_base, frame0, mtu );

    /* queues init */

    if( FD_UNLIKELY( xsk_burst==0UL ) ) {
      FD_LOG_WARNING(( "invalid xsk_burst" ));
      return 1;
    }

    fill_prod_p = fill.prod;
    fill_cons_p = fill.cons;
    rx_prod_p   = rx.prod;
    rx_cons_p   = rx.cons;

    fill_prod = fill_prod_p[0];
    fill_cons = fill_cons_p[0];
    rx_prod   = rx_prod_p  [0];
    rx_cons   = rx_cons_p  [0];
    seq_flush = seq + xsk_burst;

    /* housekeeping init */

    if( lazy<=0L ) lazy = fd_tempo_lazy_default( mcache_depth );
    FD_LOG_INFO(( "Configuring housekeeping (lazy %li ns)", lazy ));

    async_min = fd_tempo_async_min( lazy, 1UL /*event_cnt*/, (float)tick_per_ns );
    if( FD_UNLIKELY( !async_min ) ) { FD_LOG_WARNING(( "bad lazy" )); return 1; }

    /* Initial poll */

    if( cfg->poll_mode==FDGEN_XSK_POLL_MODE_WAKEUP ) {
      sendto( cfg->xsk_fd, NULL, 0, MSG_DONTWAIT, NULL, 0 );
      XSK_SYNC();
    }

  } while(0);

  FD_LOG_INFO(( "Running AF_XDP recv (orig %lu)", orig ));
  fd_cnc_signal( cnc, FD_CNC_SIGNAL_RUN );
  long then = fd_tickcount();
  long now  = then;
  for(;;) {

    /* Do housekeeping at a low rate in the background */

    if( FD_UNLIKELY( (now-then)>=0L ) ) {
      XSK_SYNC();

      /* Send synchronization info */
      fd_mcache_seq_update( sync, seq );

      /* Send diagnostic info */
      fd_cnc_heartbeat( cnc, now );
      FD_COMPILER_MFENCE();
      cnc_diag->in_backp   = cnc_diag_in_backp;
      cnc_diag->backp_cnt += cnc_diag_backp_cnt;
      cnc_diag->pub_cnt   += cnc_diag_pcap_pub_cnt;
      cnc_diag->pub_sz    += cnc_diag_pcap_pub_sz;
      cnc_diag->fr_cons    = fill_cons;
      cnc_diag->fr_prod    = fill_prod;
      cnc_diag->rx_cons    = rx_cons;
      cnc_diag->rx_prod    = rx_prod;
      FD_COMPILER_MFENCE();
      cnc_diag_backp_cnt    = 0UL;
      cnc_diag_pcap_pub_cnt = 0UL;
      cnc_diag_pcap_pub_sz  = 0UL;

      /* Receive command-and-control signals */
      ulong s = fd_cnc_signal_query( cnc );
      if( FD_UNLIKELY( s!=FD_CNC_SIGNAL_RUN ) ) {
        if( FD_LIKELY( s==FD_CNC_SIGNAL_HALT ) ) break;
        fd_cnc_signal( cnc, FD_CNC_SIGNAL_RUN );
      }

      /* Reload housekeeping timer */
      then = now + (long)fd_tempo_async_reload( rng, async_min );
    }

    if( FD_VOLATILE_CONST( rx.flags[0] ) & XDP_RING_NEED_WAKEUP ) {
      XSK_SYNC();
      xsk_poll_recv( cfg->xsk_fd );
      XSK_SYNC();
    }

    /* Check if there is any new fragment received */

    int avail = (int)( rx_prod - rx_cons );
    if( FD_UNLIKELY( avail<=0 ) ) {

      if( cfg->poll_mode == FDGEN_XSK_POLL_MODE_BUSY_SYNC ) {
        FD_COMPILER_MFENCE();
        FD_VOLATILE( rx_cons_p  [0] ) = rx_cons;
        FD_VOLATILE( fill_prod_p[0] ) = fill_prod;
        FD_COMPILER_MFENCE();

        xsk_poll_recv( cfg->xsk_fd );

        FD_COMPILER_MFENCE();
        fill_cons = FD_VOLATILE_CONST( fill_cons_p[0] );
        rx_prod   = FD_VOLATILE_CONST( rx_prod_p  [0] );
        FD_COMPILER_MFENCE();
      } else {
        XSK_SYNC();
      }

      cnc_diag_backp_cnt += (ulong)!cnc_diag_in_backp;
      cnc_diag_in_backp   = 1;
      FD_SPIN_PAUSE();
      now = fd_tickcount();
      continue;
    }
    cnc_diag_in_backp = 0;

    struct xdp_desc * rx_frag = __builtin_assume_aligned( rx.packet_ring + (rx_cons & (rx.depth-1)), 16UL );

    /* Check if there is sufficient fill ring space */

    if( FD_UNLIKELY( fill_prod - fill_cons >= fill.depth ) ) {
      fill_cons = FD_VOLATILE_CONST( fill_cons_p[0] );
      FD_VOLATILE( rx_cons_p[0] ) = rx_cons;
      now = fd_tickcount();
      continue;
    }

    ulong volatile * fill_line = fill.frame_ring + (fill_prod & (fill.depth-1));

    /* Catch the frag we are about to replace */

    fd_frag_meta_t const * mline = mcache + fd_mcache_line_idx( seq, mcache_depth );

    uint  free_chunk    = mline->chunk;
    ulong free_umem_off = fd_chunk_to_umem( base, base, free_chunk );
          free_umem_off = fd_ulong_align_dn( free_umem_off, 2048UL );

    /* Create mcache entry */

    ulong chunk = fd_umem_to_chunk( base, umem_base, rx_frag->addr );
    ulong sz    = rx_frag->len;
    ulong sig   = 0UL;  /* TODO */
    ulong ctl   = fd_frag_meta_ctl( orig, 1 /* som */, 1 /* eom */, 0 /* err */ );

    now = fd_tickcount();
    ulong tspub  = fd_frag_meta_ts_comp( now );
    ulong tsorig = tspub;

    /* Write frag */

    fd_mcache_publish_sse( mcache, mcache_depth, seq, sig, chunk, sz, ctl, tsorig, tspub );
    FD_COMPILER_MFENCE();
    fill_line[0] = free_umem_off;
    FD_COMPILER_MFENCE();

    /* Windup for the next iteration and accumulate diagnostics */

    rx_cons   = rx_cons   + 1U;
    fill_prod = fill_prod + 1U;
    seq       = fd_seq_inc( seq, 1UL );
    cnc_diag_pcap_pub_cnt++;
    cnc_diag_pcap_pub_sz += sz;
  }

  do {

    FD_LOG_INFO(( "Halted net_xsk_rx" ));
    fd_cnc_signal( cnc, FD_CNC_SIGNAL_BOOT );

  } while(0);

  return 0;
}
