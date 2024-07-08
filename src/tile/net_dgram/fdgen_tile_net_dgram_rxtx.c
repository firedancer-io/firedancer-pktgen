#define _GNU_SOURCE
#include "fdgen_tile_net_dgram_rxtx.h"

#include <sys/epoll.h>
#include <sys/socket.h>

#include <firedancer/tango/fd_tango_base.h>
#include <firedancer/tango/cnc/fd_cnc.h>
#include <firedancer/tango/mcache/fd_mcache.h>
#include <firedancer/tango/dcache/fd_dcache.h>
#include <firedancer/tango/tempo/fd_tempo.h>

int
fdgen_tile_net_dgram_rxtx_run( fdgen_tile_net_dgram_rxtx_cfg * cfg ) {

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

  /* cnc state */
  fdgen_tile_net_xsk_rx_diag_t * cnc_diag;
  ulong   cnc_diag_in_backp;      /* is the run loop currently backpressured by fill ring, in [0,1] */
  ulong   cnc_diag_backp_cnt;     /* Accumulates number of transitions of tile to backpressured between housekeeping events */
  ulong   cnc_diag_pcap_pub_cnt;  /* Accumulates number of XDP frags published between housekeeping events */
  ulong   cnc_diag_pcap_pub_sz;   /* Accumulates XDP payload bytes publised between housekeeping events */

  /* in frag stream state */
  ulong   in_mcache_depth;
  ulong   in_seq;

  /* out frag stream state */
  ulong   out_mcache_depth; /* ==fd_mcache_depth( mcache ), depth of the mcache / positive integer power of 2 */
  ulong * out_sync;         /* ==fd_mcache_seq_laddr( mcache ), local addr where mcache sync info is published */
  ulong   out_seq;          /* frag sequence number to publish */

  /* housekeeping state */
  ulong async_min; /* minimum number of ticks between processing a housekeeping event, positive integer power of 2 */

  /* epoll state */
  struct epoll_event events[ FD_TILE_NET_DGRAM_SOCKET_MAX ];
  int                event_idx;

  do {

    FD_LOG_INFO(( "Booting net_dgram_rxtx" ));

    /* cnc state init */

    if( FD_UNLIKELY( !cnc ) ) { FD_LOG_WARNING(( "NULL cnc" )); return 1; }
    if( FD_UNLIKELY( fd_cnc_app_sz( cnc )<sizeof(fdgen_tile_net_dgram_rxtx_cfg_t) ) ) { FD_LOG_WARNING(( "undersz cnc diag" )); return 1; }
    if( FD_UNLIKELY( fd_cnc_signal_query( cnc )!=FD_CNC_SIGNAL_BOOT ) ) { FD_LOG_WARNING(( "already booted" )); return 1; }

    cnc_diag = fd_cnc_app_laddr( cnc );

    cnc_diag_in_backp      = 1UL;
    cnc_diag_backp_cnt     = 0UL;
    cnc_diag_pcap_pub_cnt  = 0UL;
    cnc_diag_pcap_pub_sz   = 0UL;

  } while(0);

  FD_LOG_INFO(( "Running datagram socket driver (orig %lu)", orig ));
  fd_cnc_signal( cnc, FD_CNC_SIGNAL_RUN );
  long then = fd_tickcount();
  long now  = then;
  for(;;) {

    /* Do housekeeping at a low rate in the background */

    if( FD_UNLIKELY( (now-then)>=0L ) ) {
      /* Send synchronization info */
      fd_mcache_seq_update( sync, seq );

      /* Send diagnostic info */
      fd_cnc_heartbeat( cnc, now );
      FD_COMPILER_MFENCE();
      cnc_diag->in_backp   = cnc_diag_in_backp;
      cnc_diag->backp_cnt += cnc_diag_backp_cnt;
      cnc_diag->pub_cnt   += cnc_diag_pcap_pub_cnt;
      cnc_diag->pub_sz    += cnc_diag_pcap_pub_sz;
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

    fd_frag_meta_t const * mline = fd_mcache_line_idx( in_seq, in_mcache_depth );

    ulong seq_found = fd_frag_meta_seq_query( mline );
    if( FD_UNLIKELY( fd_seq_gt( seq_found, seq ) ) ) {
      /* handle overrun */
    } else if( fd_seq_eq( seq_found, seq ) ) {
      /* forward */
    }

    if( event_idx>=0 ) {

      /* RX available, forward to mcache */

    } else {

      /* No RX available, poll for updates (level activated) */
      event_idx = epoll_wait( epoll_fd, events, FD_TILE_NET_DGRAM_SOCKET_MAX ...
      if( event_idx<0 ) {
        int err = errno;
        if( FD_LIKELY( err==EINTR ) ) continue;
        FD_LOG_WARNING(( "epoll_wait failed (%i-%s)", err, fd_io_strerror( err ) ));
        return 1;
      }

    }
  }

  do {

    FD_LOG_INFO(( "Halted net_dgram_rxtx" ));
    fd_cnc_signal( cnc, FD_CNC_SIGNAL_BOOT );

  } while(0);

  return 0;
}
