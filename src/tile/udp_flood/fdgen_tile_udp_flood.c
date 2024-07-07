#include "fdgen_tile_udp_flood.h"

#include <firedancer/tango/mcache/fd_mcache.h>
#include <firedancer/tango/dcache/fd_dcache.h>
#include <firedancer/tango/fctl/fd_fctl.h>
#include <firedancer/tango/fseq/fd_fseq.h>
#include <firedancer/tango/tempo/fd_tempo.h>
#include <firedancer/util/net/fd_eth.h>
#include <firedancer/util/net/fd_ip4.h>
#include <firedancer/util/net/fd_udp.h>

int
fdgen_tile_udp_flood_run( fdgen_tile_udp_flood_cfg_t * cfg ) {

  if( FD_UNLIKELY( !cfg ) ) { FD_LOG_WARNING(( "NULL cfg" )); return 1; }

  /* load config */
  fd_cnc_t *       cnc         = cfg->cnc;
  ulong            orig        = cfg->orig;
  fd_rng_t *       rng         = cfg->rng;
  fd_frag_meta_t * mcache      = cfg->mcache;
  uchar *          dcache      = cfg->dcache;
  long             lazy        = cfg->lazy;
  ulong *          out_fseq    = cfg->out_fseq;
  ulong            cr_max      = cfg->cr_max;

  /* cnc state */
  fdgen_tile_udp_flood_diag_t * cnc_diag;  /* ==fd_cnc_app_laddr( cnc ), local address of the replay tile cnc diagnostic region */
  ulong   cnc_diag_in_backp;      /* is the run loop currently backpressured by one or more of the outs, in [0,1] */
  ulong   cnc_diag_backp_cnt;     /* Accumulates number of transitions of tile to backpressured between housekeeping events */
  ulong   cnc_diag_pcap_pub_cnt;  /* Accumulates number of pcap packets published between housekeeping events */
  ulong   cnc_diag_pcap_pub_sz;   /* Accumulates pcap payload bytes published between housekeeping events */

  /* out frag stream state */
  ulong   depth;
  ulong * sync;
  ulong   seq;

  void * base;
  ulong  chunk0;
  ulong  wmark;
  ulong  chunk;

  /* flow control state */
  uchar       fctl_mem[ FD_FCTL_FOOTPRINT( 1 ) ] __attribute__((aligned(FD_FCTL_ALIGN)));
  fd_fctl_t * fctl;
  ulong       cr_avail;
  ulong       slow_diag_cnt[1];

  /* housekeeping state */
  ulong async_min; /* minimum number of ticks between processing a housekeeping event, positive integer power of 2 */

  /* Template network structures */
  fd_eth_hdr_t tpl_eth_hdr = { .net_type = FD_ETH_HDR_TYPE_IP };
  memcpy( tpl_eth_hdr.dst, cfg->dst_mac, 6 );
  memcpy( tpl_eth_hdr.src, cfg->src_mac, 6 );
  fd_ip4_hdr_t tpl_ip4_hdr = {
    .verihl       = FD_IP4_VERIHL( 4, 5 ),
    .net_tot_len  = (ushort)fd_ushort_bswap( (ushort)( sizeof(fd_ip4_hdr_t) + sizeof(fd_udp_hdr_t) ) ),
    .net_frag_off = (ushort)fd_ushort_bswap( FD_IP4_HDR_FRAG_OFF_DF ),
    .ttl          = 0,
    .protocol     = FD_IP4_HDR_PROTOCOL_UDP
  };
  fd_udp_hdr_t tpl_udp_hdr = {
    .net_sport = (ushort)fd_ushort_bswap( cfg->src_port ),
    .net_dport = (ushort)fd_ushort_bswap( cfg->dst_port ),
    .net_len   = sizeof(fd_udp_hdr_t),
    .check     = 0
  };
  ulong const sz = sizeof(fd_eth_hdr_t) + sizeof(fd_ip4_hdr_t) + sizeof(fd_udp_hdr_t);

  do {

    FD_LOG_INFO(( "Booting udp_flood" ));

    /* cnc state init */

    if( FD_UNLIKELY( !cnc ) ) { FD_LOG_WARNING(( "NULL cnc" )); return 1; }
    if( FD_UNLIKELY( fd_cnc_app_sz( cnc )<64UL ) ) { FD_LOG_WARNING(( "cnc app sz must be at least 64" )); return 1; }
    if( FD_UNLIKELY( fd_cnc_signal_query( cnc )!=FD_CNC_SIGNAL_BOOT ) ) { FD_LOG_WARNING(( "already booted" )); return 1; }

    cnc_diag = fd_cnc_app_laddr( cnc );

    /* in_backp==1, backp_cnt==0 indicates waiting for initial credits,
       cleared during first housekeeping if credits available */
    cnc_diag_in_backp      = 1UL;
    cnc_diag_backp_cnt     = 0UL;
    cnc_diag_pcap_pub_cnt  = 0UL;
    cnc_diag_pcap_pub_sz   = 0UL;

    /* out frag stream init */

    if( FD_UNLIKELY( !mcache ) ) { FD_LOG_WARNING(( "NULL mcache" )); return 1; }
    depth = fd_mcache_depth    ( mcache );
    sync  = fd_mcache_seq_laddr( mcache );

    seq = fd_mcache_seq_query( sync ); /* FIXME: ALLOW OPTION FOR MANUAL SPECIFICATION */

    if( FD_UNLIKELY( !dcache ) ) { FD_LOG_WARNING(( "NULL dcache" )); return 1; }

    base = fd_wksp_containing( dcache );
    if( FD_UNLIKELY( !base ) ) { FD_LOG_WARNING(( "fd_wksp_containing failed" )); return 1; }

    if( FD_UNLIKELY( !fd_dcache_compact_is_safe( base, dcache, sz, depth ) ) ) {
      FD_LOG_WARNING(( "--dcache not compatible with wksp base, --pkt-max and --mcache depth" ));
      return 1;
    }

    chunk0 = fd_dcache_compact_chunk0( base, dcache );
    wmark  = fd_dcache_compact_wmark ( base, dcache, sz );
    chunk  = FD_VOLATILE_CONST( cnc_diag->chunk_idx );
    if( FD_UNLIKELY( !((chunk0<=chunk) & (chunk<=wmark)) ) ) {
      chunk = chunk0;
      FD_LOG_INFO(( "out of bounds cnc chunk index; overriding initial chunk to chunk0" ));
    }
    FD_LOG_INFO(( "chunk %lu", chunk ));

    /* out flow control init */

    fctl = fd_fctl_join( fd_fctl_new( fctl_mem, 1 ) );
    if( FD_UNLIKELY( !fctl ) ) { FD_LOG_WARNING(( "join failed" )); return 1; }

    if( out_fseq ) {
      /* Assumes lag_max==depth */
      /* FIXME: CONSIDER ADDING LAG_MAX THIS TO FSEQ AS A FIELD? */
      ulong * slow_diag_cnt_p = cfg->out_fseq_diag_slow_cnt;
      if( !slow_diag_cnt_p ) slow_diag_cnt_p = slow_diag_cnt;
      if( FD_UNLIKELY( !fd_fctl_cfg_rx_add( fctl, depth, out_fseq, slow_diag_cnt_p ) ) ) {
        FD_LOG_WARNING(( "fd_fctl_cfg_rx_add failed" ));
        return 1;
      }
    }

    /* cr_burst is 1 because we only send at most 1 fragment metadata
       between checking cr_avail.  We use defaults for cr_resume and
       cr_refill (and possible cr_max if the user wanted to use defaults
       here too). */

    if( FD_UNLIKELY( !fd_fctl_cfg_done( fctl, 1UL, cr_max, 0UL, 0UL ) ) ) {
      FD_LOG_WARNING(( "fd_fctl_cfg_done failed" ));
      return 1;
    }
    FD_LOG_INFO(( "cr_burst %lu cr_max %lu cr_resume %lu cr_refill %lu",
                  fd_fctl_cr_burst( fctl ), fd_fctl_cr_max( fctl ), fd_fctl_cr_resume( fctl ), fd_fctl_cr_refill( fctl ) ));

    cr_max   = fd_fctl_cr_max( fctl );
    cr_avail = 0UL; /* Will be initialized by run loop */

    /* housekeeping init */

    if( lazy<=0L ) lazy = fd_tempo_lazy_default( cr_max );
    FD_LOG_INFO(( "Configuring housekeeping (lazy %li ns)", lazy ));

    async_min = fd_tempo_async_min( lazy, 1UL /*event_cnt*/, (float)fd_tempo_tick_per_ns( NULL ) );
    if( FD_UNLIKELY( !async_min ) ) { FD_LOG_WARNING(( "bad lazy" )); return 1; }

  } while(0);

  FD_LOG_INFO(( "Running udp_flood (orig %lu)", orig ));
  fd_cnc_signal( cnc, FD_CNC_SIGNAL_RUN );
  long then = fd_tickcount();
  long now  = then;
  for(;;) {

    /* Do housekeeping at a low rate in the background */
    if( FD_UNLIKELY( (now-then)>=0L ) ) {

      /* Send synchronization info */
      fd_mcache_seq_update( sync, seq );

      fd_cnc_heartbeat( cnc, now );
      FD_COMPILER_MFENCE();
      cnc_diag->in_backp   = cnc_diag_in_backp;
      cnc_diag->backp_cnt += cnc_diag_backp_cnt;
      cnc_diag->chunk_idx  = chunk;
      cnc_diag->pub_cnt   += cnc_diag_pcap_pub_cnt;
      cnc_diag->pub_sz    += cnc_diag_pcap_pub_sz;
      FD_COMPILER_MFENCE();
      cnc_diag_backp_cnt     = 0UL;
      cnc_diag_pcap_pub_cnt  = 0UL;
      cnc_diag_pcap_pub_sz   = 0UL;

      /* Receive command-and-control signals */
      ulong s = fd_cnc_signal_query( cnc );
      if( FD_UNLIKELY( s!=FD_CNC_SIGNAL_RUN ) ) {
        if( FD_LIKELY( s==FD_CNC_SIGNAL_HALT ) ) break;
        fd_cnc_signal( cnc, FD_CNC_SIGNAL_RUN );
      }

      /* Receive flow control credits */
      cr_avail = fd_fctl_tx_cr_update( fctl, cr_avail, seq );

      /* Reload housekeeping timer */
      then = now + (long)fd_tempo_async_reload( rng, async_min );
    }

    /* Check if we are backpressured.  If so, count any transition into
       a backpressured regime and spin to wait for flow control credits
       to return.  We don't do a fully atomic update here as it is only
       diagnostic and it will still be correct the usual case where
       individual diagnostic counters aren't used by writers in
       different threads of execution.  We only count the transition
       from not backpressured to backpressured. */

    if( FD_UNLIKELY( !cr_avail ) ) {
      cnc_diag_backp_cnt += (ulong)!cnc_diag_in_backp;
      cnc_diag_in_backp   = 1UL;
      FD_SPIN_PAUSE();
      now = fd_tickcount();
      continue;
    }
    cnc_diag_in_backp = 0UL;

    /* Load the next packet directly into the dcache at chunk */

    uchar *        out     = fd_chunk_to_laddr( base, chunk );
    fd_eth_hdr_t * eth_hdr = fd_type_pun( out );  out += sizeof(fd_eth_hdr_t);
    fd_ip4_hdr_t * ip4_hdr = fd_type_pun( out );  out += sizeof(fd_ip4_hdr_t);
    fd_udp_hdr_t * udp_hdr = fd_type_pun( out );  out += sizeof(fd_udp_hdr_t);
    eth_hdr[0] = tpl_eth_hdr;
    ip4_hdr[0] = tpl_ip4_hdr;
    udp_hdr[0] = tpl_udp_hdr;
    ip4_hdr->net_id = (ushort)fd_ushort_bswap( (ushort)seq );
    ip4_hdr->check  = fd_ip4_hdr_check_fast( ip4_hdr );

    ulong sig = 0UL; /* TODO */
    ulong ctl = fd_frag_meta_ctl( orig, 1 /*som*/, 1 /*eom*/, 0 /*err*/ );

    now = fd_tickcount();
    ulong tsorig = fd_frag_meta_ts_comp( now );
    ulong tspub  = tsorig;
    fd_mcache_publish( mcache, depth, seq, sig, chunk, sz, ctl, tsorig, tspub );

    /* Windup for the next iteration and accumulate diagnostics */

    chunk = fd_dcache_compact_next( chunk, sz, chunk0, wmark );
    seq   = fd_seq_inc( seq, 1UL );
    cr_avail--;
    cnc_diag_pcap_pub_cnt++;
    cnc_diag_pcap_pub_sz += sz;
  }

  do {

    FD_LOG_INFO(( "Halting udp_flood" ));

    FD_LOG_INFO(( "Destroying fctl" ));
    fd_fctl_delete( fd_fctl_leave( fctl ) );

    FD_LOG_INFO(( "Halted udp_flood" ));
    fd_cnc_signal( cnc, FD_CNC_SIGNAL_BOOT );

  } while(0);

  return 0;
}
