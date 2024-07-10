#include "fdgen_tile_net_xsk_tx.h"

#include <firedancer/tango/fd_tango.h>
#include <firedancer/tango/mcache/fd_mcache.h>

int
fdgen_tile_net_xsk_tx_run( fdgen_tile_net_xsk_tx_cfg_t * cfg ) {

  if( FD_UNLIKELY( !cfg ) ) { FD_LOG_WARNING(( "NULL cfg" )); return 1; }

  /* load config */

  fd_cnc_t *       cnc         = cfg->cnc;
  ulong            orig        = cfg->orig;
  fd_rng_t *       rng         = cfg->rng;
  fd_frag_meta_t * mcache      = cfg->mcache;
  uchar *          dcache      = cfg->dcache;
  long             lazy        = cfg->lazy;

  /* in frag stream state */
  ulong         mcache_depth; /* ==fd_mcache_depth( mcache ), depth of the mcache / positive integer power of 2 */
  ulong const * sync;         /* ==fd_mcache_seq_laddr( mcache ), local addr where mcache sync info is published */
  ulong         seq;

  do {

    FD_LOG_INFO(( "Booting net_xsk_tx" ));

    /* cnc state init */

    if( FD_UNLIKELY( !cnc ) ) { FD_LOG_WARNING(( "NULL cnc" )); return 1; }
    if( FD_UNLIKELY( fd_cnc_app_sz( cnc )<64UL ) ) { FD_LOG_WARNING(( "cnc app sz must be at least 64" )); return 1; }
    if( FD_UNLIKELY( fd_cnc_signal_query( cnc )!=FD_CNC_SIGNAL_BOOT ) ) { FD_LOG_WARNING(( "already booted" )); return 1; }

    /* out frag stream init */

    if( FD_UNLIKELY( !mcache ) ) { FD_LOG_WARNING(( "NULL mcache" )); return 1; }
    mcache_depth = fd_mcache_depth          ( mcache );
    sync         = fd_mcache_seq_laddr_const( mcache );

    seq          = fd_mcache_seq_query( sync );

  } while(0);

  fd_frag_meta_t const * mline = mcache + fd_mcache_line_idx( seq, mcache_depth );

  ulong ovrnp_cnt = 0UL;
  ulong ovrnr_cnt = 0UL;

  ulong async_min = 1UL << lazy;

  fd_cnc_signal( cnc, FD_CNC_SIGNAL_RUN );

  long now  = fd_tickcount();
  long then = now; /* Do housekeeping on first iteration */
  for(;;) {

    /* Do housekeeping in background */
    if( FD_UNLIKELY( (now-then)>=0L ) ) {

      /* Send monitoring info */
      long now = fd_log_wallclock();
      fd_cnc_heartbeat( cnc, now );

      /* Receive command-and-control signals */
      ulong s = fd_cnc_signal_query( cnc );
      if( FD_UNLIKELY( s!=FD_CNC_SIGNAL_RUN ) ) {
        if( FD_LIKELY( s==FD_CNC_SIGNAL_HALT ) ) break;
        char buf[ FD_CNC_SIGNAL_CSTR_BUF_MAX ];
        FD_LOG_WARNING(( "Unexpected signal %s (%lu) received; trying to resume", fd_cnc_signal_cstr( s, buf ), s ));
        fd_cnc_signal( cnc, FD_CNC_SIGNAL_RUN );
      }

      /* Reload housekeeping timer */
      then = now + (long)fd_tempo_async_reload( rng, async_min );
      continue;
    }

    ulong seq_found = fd_frag_meta_seq_query( mline );
    long  diff      = fd_seq_diff( seq_found, seq );
    if( FD_UNLIKELY( diff ) ) {
      if( FD_LIKELY( diff<0L ) ) { /* caught up */
        FD_SPIN_PAUSE();
        now = fd_tickcount();
        continue;
      }
      ovrnp_cnt++;
      seq = seq_found;
    }

    now = fd_tickcount();

    ulong sz = (ulong)mline->sz;

    seq_found = fd_frag_meta_seq_query( mline );
    if( FD_UNLIKELY( fd_seq_ne( seq_found, seq ) ) ) {
      ovrnr_cnt++;
      seq = seq_found;
      continue;
    }

    /* Wind up for the next iteration */
    seq   = fd_seq_inc( seq, 1UL );
    mline = mcache + fd_mcache_line_idx( seq, mcache_depth );
  }

  fd_cnc_signal( cnc, FD_CNC_SIGNAL_BOOT );

  return 0;
}
