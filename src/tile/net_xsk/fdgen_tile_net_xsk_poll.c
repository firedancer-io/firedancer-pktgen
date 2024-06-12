#include "fdgen_tile_net_xsk_poll.h"

#include <errno.h>
#include <poll.h> /* poll(2) */
#include <linux/if_xdp.h>
#include <sys/socket.h>

#include <firedancer/tango/tempo/fd_tempo.h>

int
fdgen_tile_net_xsk_poll_run( fdgen_tile_net_xsk_poll_cfg_t * cfg ) {

  if( FD_UNLIKELY( !cfg ) ) { FD_LOG_WARNING(( "NULL cfg" )); return 1; }

  /* load config */
  fd_cnc_t * cnc         = cfg->cnc;
  int        xsk_fd      = cfg->xsk_fd;
  fd_rng_t * rng         = cfg->rng;
  long       lazy        = cfg->lazy;
  double     tick_per_ns = cfg->tick_per_ns;

  /* housekeeping state */
  ulong async_min; /* minimum number of ticks between processing a housekeeping event, positive integer power of 2 */

  do {

    FD_LOG_INFO(( "Booting net_xsk_rx" ));

    /* cnc state init */

    if( FD_UNLIKELY( !cnc ) ) { FD_LOG_WARNING(( "NULL cnc" )); return 1; }
    if( FD_UNLIKELY( fd_cnc_app_sz( cnc )<64UL ) ) { FD_LOG_WARNING(( "cnc app sz must be at least 64" )); return 1; }
    if( FD_UNLIKELY( fd_cnc_signal_query( cnc )!=FD_CNC_SIGNAL_BOOT ) ) { FD_LOG_WARNING(( "already booted" )); return 1; }

    /* xsk init */

    if( FD_UNLIKELY( xsk_fd<0 ) ) { FD_LOG_WARNING(( "invalid xsk fd" )); return 1; }

    /* housekeeping init */

    if( lazy<=0L ) { FD_LOG_WARNING(( "invalid lazy" )); return 1; }
    FD_LOG_INFO(( "Configuring housekeeping (lazy %li ns)", lazy ));

    async_min = fd_tempo_async_min( lazy, 1UL /*event_cnt*/, (float)tick_per_ns );
    if( FD_UNLIKELY( !async_min ) ) { FD_LOG_WARNING(( "bad lazy" )); return 1; }

  } while(0);

  int fail = 0;

  FD_LOG_INFO(( "Running AF_XDP poll" ));
  fd_cnc_signal( cnc, FD_CNC_SIGNAL_RUN );
  long then = fd_tickcount();
  long now  = then;
  for(;;) {

    /* Do housekeeping at a low rate in the background */

    if( FD_UNLIKELY( (now-then)>=0L ) ) {

      /* Receive command-and-control signals */
      ulong s = fd_cnc_signal_query( cnc );
      if( FD_UNLIKELY( s!=FD_CNC_SIGNAL_RUN ) ) {
        if( FD_LIKELY( s==FD_CNC_SIGNAL_HALT ) ) break;
        fd_cnc_signal( cnc, FD_CNC_SIGNAL_RUN );
      }

      //struct xdp_statistics stats[1];
      //uint optlen = sizeof(*stats);
	    //FD_TEST( 0==getsockopt( xsk_fd, SOL_XDP, XDP_STATISTICS, stats, &optlen ) );
      //FD_LOG_NOTICE(( "stats drop=%lu rx_invalid=%lu rx_full=%lu fill_empty=%lu",
      //                stats->rx_dropped, stats->rx_invalid_descs, stats->rx_ring_full, stats->rx_fill_ring_empty_descs ));

      /* Reload housekeeping timer */
      then = now + (long)fd_tempo_async_reload( rng, async_min );
    }

    struct pollfd polls[1] = {{
      .fd     = xsk_fd,
      .events = POLLIN
    }};
    int poll_res = poll( polls, 1, 0 );
    if( FD_UNLIKELY( poll_res<0 ) ) {
      FD_LOG_WARNING(( "poll(AF_XDP) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
      fail = 1;
      break;
    }

    now = fd_tickcount();
  }

  do {
    FD_LOG_INFO(( "Halted net_xsk_poll" ));
    fd_cnc_signal( cnc, FD_CNC_SIGNAL_BOOT );
  } while(0);

  return fail ? 1 : 0;
}
