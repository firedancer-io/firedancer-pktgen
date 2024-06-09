#include "fdgen_tile_net_xsk.h"
#include "fdgen_tile_net_xsk_rx.h"

#include <assert.h>
#include <linux/if_xdp.h>

#include <firedancer/tango/fd_tango_base.h>
#include <firedancer/tango/cnc/fd_cnc.h>
#include <firedancer/tango/mcache/fd_mcache.h>
#include <firedancer/tango/dcache/fd_dcache.h>
#include <firedancer/tango/tempo/fd_tempo.h>

/* Address translation between fd_tango and XSK UMEM */

FD_FN_CONST static inline ulong
fd_chunk_to_umem( void * chunk0,
                  void * umem_base,
                  ulong  chunk ) {
  ulong laddr = (ulong)fd_chunk_to_laddr( chunk0, chunk );
  return laddr - (ulong)umem_base;
}

FD_FN_CONST static inline ulong
fd_umem_to_chunk( void * chunk0,
                  void * umem_base,
                  ulong  umem_off ) {
  ulong laddr = (ulong)umem_base + umem_off;
  return fd_laddr_to_chunk( chunk0, (void *)laddr );
}

/* init_rings sets up the initial allocation of frames between mcache
   and xsk fill ring.  This is essential as the message queues also
   serve as FIFO allocators. */

static void
init_rings( fd_frag_meta_t *   mcache,
            uchar *            dcache,
            uchar *            dcache_base,
            fdgen_xsk_ring_t * fill ) {

  /* Assign frames to mcache (2^n) */

  uint  chunk        = (uint)fd_laddr_to_chunk( dcache_base, dcache );
  ulong mcache_depth = fd_mcache_depth( mcache );

  for( ulong j=0UL; j<mcache_depth; j++ ) {
    mcache[j].ctl    = fd_frag_meta_ctl( 0, 0, 0, /* err */ 1 );
    mcache[j].chunk  = chunk;
    chunk           += (FDGEN_XSK_FRAME_SZ >> FD_CHUNK_LG_SZ);
  }

  /* Assign frames to fill ring (2^n - 1) */

  ulong   fill_off   = (ulong)fd_chunk_to_laddr( dcache_base, chunk ) - (ulong)fill->mem;
  ulong * fill_ring  = fill->frame_ring;
  ulong   fill_depth = fill->depth - 1UL;

  for( ulong j=0UL; j<fill_depth; j++ ) {
    fill_ring[j]  = fill_off;
    fill_off     += FDGEN_XSK_FRAME_SZ;
  }

  FD_COMPILER_MFENCE();
  FD_VOLATILE( fill->prod[0] ) = (uint)fill_depth;
  FD_COMPILER_MFENCE();

}

int
fdgen_tile_net_xsk_run( fdgen_tile_net_xsk_rx_cfg_t * cfg ) {

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
  ulong            xsk_burst   = cfg->xsk_burst;

  /* cnc state */
  ulong * cnc_diag;
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

  /* cached XSK queue states */
  uint   fill_prod;  /* owned */
  uint   fill_cons;  /* stale */
  uint   rx_prod;    /* stale */
  uint   rx_cons;    /* owned */
  ulong  seq_flush;  /* flush dirty & prefetch read cache at this seq */

  do {

    FD_LOG_INFO(( "Booting net_xsk_rx" ));

    /* cnc state init */

    if( FD_UNLIKELY( !cnc ) ) { FD_LOG_WARNING(( "NULL cnc" )); return 1; }
    if( FD_UNLIKELY( fd_cnc_app_sz( cnc )<64UL ) ) { FD_LOG_WARNING(( "cnc app sz must be at least 64" )); return 1; }
    if( FD_UNLIKELY( fd_cnc_signal_query( cnc )!=FD_CNC_SIGNAL_BOOT ) ) { FD_LOG_WARNING(( "already booted" )); return 1; }

    cnc_diag = (ulong *)fd_cnc_app_laddr( cnc );

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

    if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)dcache, FDGEN_XSK_FRAME_SZ ) ) ) {
      FD_LOG_WARNING(( "misaligned dcache" ));
      return 1;
    }

    if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)base, FDGEN_XSK_FRAME_SZ ) ) ) {
      FD_LOG_WARNING(( "misaligned base" ));
      return 1;
    }

    /* check buffer space */

    ulong total_depth;
    if( FD_UNLIKELY( !__builtin_uaddl_overflow( mcache_depth, cfg->ring_fr.depth, &total_depth ) ) ) {
      FD_LOG_WARNING(( "mcache+fill depth overflow" ));
      return 1;
    }

    ulong total_bufsz;
    if( FD_UNLIKELY( !__builtin_umull_overflow( total_depth, FDGEN_XSK_FRAME_SZ, &total_bufsz ) ) ) {
      FD_LOG_WARNING(( "mcache+fill depth overflow" ));
      return 1;
    }

    ulong dcache_data_sz = fd_dcache_data_sz( dcache );
    if( FD_UNLIKELY( dcache_data_sz < total_bufsz ) ) {
      FD_LOG_WARNING(( "dcache data sz too small (have %#lx, need %#lx)", dcache_data_sz, total_bufsz ));
      return 1;
    }

    /* mcache init */

    init_rings( mcache, dcache, base, &cfg->ring_fr );

    /* queues init */

    if( FD_UNLIKELY( xsk_burst==0UL || xsk_burst > fill.depth/2 ) ) {
      FD_LOG_WARNING(( "invalid xsk_burst: %lu not in [0,%u)", xsk_burst, fill.depth/2 ));
      return 1;
    }

    fill_prod = FD_VOLATILE_CONST( fill.prod[0] );
    fill_cons = FD_VOLATILE_CONST( fill.cons[0] );
    rx_prod   = FD_VOLATILE_CONST( rx.prod[0]   );
    rx_cons   = FD_VOLATILE_CONST( rx.cons[0]   );
    seq_flush = seq + xsk_burst;

    /* housekeeping init */

    if( lazy<=0L ) lazy = fd_tempo_lazy_default( mcache_depth );
    FD_LOG_INFO(( "Configuring housekeeping (lazy %li ns)", lazy ));

    async_min = fd_tempo_async_min( lazy, 1UL /*event_cnt*/, (float)tick_per_ns );
    if( FD_UNLIKELY( !async_min ) ) { FD_LOG_WARNING(( "bad lazy" )); return 1; }

  } while(0);

  FD_LOG_INFO(( "Running AF_XDP recv (orig %lu)", orig ));
  fd_cnc_signal( cnc, FD_CNC_SIGNAL_RUN );
  long then = fd_tickcount();
  long now  = then;
  for(;;) {

    /* Do housekeeping at a low rate in the background */

    if( FD_UNLIKELY( (now-then)>=0L ) ) {

      /* Update XSK caches */
      FD_COMPILER_MFENCE();
      FD_VOLATILE( fill.prod[0] ) = fill_prod;
      FD_VOLATILE( rx.cons[0]   ) = rx_cons;
      fill_cons = FD_VOLATILE_CONST( fill.cons[0] );
      rx_prod   = FD_VOLATILE_CONST( rx.prod[0]   );
      FD_COMPILER_MFENCE();

      /* Send synchronization info */
      fd_mcache_seq_update( sync, seq );

      /* Send diagnostic info */
      fd_cnc_heartbeat( cnc, now );
      FD_COMPILER_MFENCE();
      cnc_diag[ FD_CNC_DIAG_IN_BACKP        ]  = cnc_diag_in_backp;
      cnc_diag[ FD_CNC_DIAG_BACKP_CNT       ] += cnc_diag_backp_cnt;
      cnc_diag[ FD_NET_XSK_CNC_DIAG_PUB_CNT ] += cnc_diag_pcap_pub_cnt;
      cnc_diag[ FD_NET_XSK_CNC_DIAG_PUB_SZ  ] += cnc_diag_pcap_pub_sz;
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

    /* Update caches */

    if( seq >= seq_flush ) {
      FD_VOLATILE( fill.prod[0] ) = fill_prod;
      FD_VOLATILE( rx.cons[0]   ) = rx_cons;
      rx_prod   = FD_VOLATILE_CONST( rx.prod[0]   );
      fill_cons = FD_VOLATILE_CONST( fill.cons[0] );
      seq_flush = seq + xsk_burst;
    }

    /* Check if there is any new fragment received */

    if( FD_UNLIKELY( rx_cons == rx_prod ) ) {
      rx_prod   = FD_VOLATILE_CONST( rx.prod[0]   );
      fill_cons = FD_VOLATILE_CONST( fill.cons[0] );
      cnc_diag_backp_cnt += (ulong)!cnc_diag_in_backp;
      cnc_diag_in_backp   = 1;
      FD_SPIN_PAUSE();
      now = fd_tickcount();
      continue;
    }
    cnc_diag_in_backp = 0;

    struct xdp_desc * rx_frag = rx.packet_ring + (rx_cons & (rx.depth-1));

    /* Check if there is sufficient fill ring space */

    uint next_fill_prod = fill_prod + 1U;
    if( FD_UNLIKELY( next_fill_prod >= fill_cons + fill.depth ) ) {
      fill_cons = FD_VOLATILE_CONST( fill.cons[0] );
      now = fd_tickcount();
      continue;
    }

    ulong * fill_line = fill.frame_ring + (fill_prod & (fill.depth-1));

    /* Catch the frag we are about to replace */

    fd_frag_meta_t const * mline = mcache + fd_mcache_line_idx( seq, mcache_depth );

    uint  free_chunk    = mline->chunk;
    ulong free_umem_off = fd_chunk_to_umem( base, umem_base, free_chunk );

    /* Create mcache entry */

    ulong chunk = fd_umem_to_chunk( base, umem_base, rx_frag->addr );
    ulong sz    = rx_frag->len;
    ulong sig   = 0UL;  /* TODO */
    ulong ctl   = fd_frag_meta_ctl( orig, 1 /* som */, 1 /* eom */, 0 /* err */ );

    now = fd_tickcount();
    ulong tspub  = fd_frag_meta_ts_comp( now );
    ulong tsorig = tspub;

    /* Write frag */

    fd_mcache_publish( mcache, mcache_depth, seq, sig, chunk, sz, ctl, tsorig, tspub );
    fill_line[0] = free_umem_off;

    /* Windup for the next iteration and accumulate diagnostics */

    fill_prod = next_fill_prod;
    rx_cons   = rx_cons + 1U;
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
