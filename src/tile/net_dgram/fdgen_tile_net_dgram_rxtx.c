#define _GNU_SOURCE
#include "fdgen_tile_net_dgram_rxtx.h"
#include "fdgen_tile_net_dgram.h"

#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <firedancer/tango/fd_tango_base.h>
#include <firedancer/tango/cnc/fd_cnc.h>
#include <firedancer/tango/mcache/fd_mcache.h>
#include <firedancer/tango/dcache/fd_dcache.h>
#include <firedancer/tango/tempo/fd_tempo.h>
#include <firedancer/util/net/fd_eth.h>
#include <firedancer/util/net/fd_ip4.h>
#include <firedancer/util/net/fd_udp.h>

/* Receive Side ********************************************************

   This tile uses recvmmsg(2) to receive a batch of `burst` packets from
   the kernel.  To ensure safe operation, at any given time the dcache
   (backing memory for packet data) is partitioned into the visible
   portion (referred to by mcache and thus may be read by consumers) and
   the invisible portion (safe to write to).  There is at least `burst`
   contiguous invisible space available before each recvmmsg() call.

   The dcache is internally divided into MTU size slots (rounded up to
   FD_CHUNK_ALIGN).  The dcache is mirrored by a mmsghdr and iovec array
   to allow for instant dispatching of recvmmsg calls.

   The start of the invisible region is located using the dcache's
   watermark (W) and the most recent published slot (R).  The watermark
   marks the first of the last `burst` slots.  If R<W, the invisible
   region starts at R+1.  If R>=W, the invisible region starts at 0.

                   +------------------+
       recvmmsg()  |     mcache       |
           \/      +------------------+
       invisible        visible
      +----------------------------------+
      |    dcache                        |
      +----------------------^--------^--+
                             W        R

   The transition between visible and invisible occurs atomically with
   each mcache publish.  After the mcache publish operations:

      +--------+             +--------+
      | mcache |             | mcache |
      +--------+             +--------+
        visible   invisble     visible
      +----------------------------------+
      |    dcache                        |
      +--------^-------------^-----------+
               R             W */

/* Transmit Side *******************************************************

   TODO */

#define HEADROOM (42UL)  /* Ethernet header, IPv4 header, UDP header */

int
fdgen_tile_net_dgram_rxtx_run( fdgen_tile_net_dgram_rxtx_cfg_t * cfg ) {

  if( FD_UNLIKELY( !cfg ) ) { FD_LOG_WARNING(( "NULL cfg" )); return 1; }

  /* load config */

  ulong            orig        = cfg->orig;
  long             lazy        = cfg->lazy;
  double           tick_per_ns = cfg->tick_per_ns;
  ulong            seq0        = cfg->seq0;
  ulong            mtu         = cfg->mtu;
  fd_cnc_t *       cnc         = cfg->cnc;
  fd_rng_t *       rng         = cfg->rng;
  fd_frag_meta_t * tx_mcache   = cfg->tx_mcache;
  uchar *          tx_base     = cfg->tx_base;
  fd_frag_meta_t * rx_mcache   = cfg->rx_mcache;
  uchar *          rx_dcache   = cfg->rx_dcache;
  uchar *          rx_base     = cfg->rx_base;
  int              epoll_fd    = cfg->epoll_fd;
  int              send_fd     = cfg->send_fd;

  /* cnc state */
  fdgen_tile_net_dgram_diag_t * cnc_diag;
  ulong   cnc_diag_backp_cnt;
  ulong   cnc_diag_tx_pub_cnt;
  ulong   cnc_diag_tx_pub_sz;
  ulong   cnc_diag_tx_filt_cnt;
  ulong   cnc_diag_rx_cnt;
  ulong   cnc_diag_rx_sz;
  ulong   cnc_diag_overnp_cnt;

  /* tx (in) frag stream state */
  ulong   tx_depth;
  ulong   tx_seq;

  /* rx (out) frag stream state */
  ulong   rx_depth; /* ==fd_mcache_depth( mcache ), depth of the mcache / positive integer power of 2 */
  ulong * rx_sync;  /* ==fd_mcache_seq_laddr( mcache ), local addr where mcache sync info is published */
  ulong   rx_seq;   /* frag sequence number to publish */

  /* housekeeping state */
  ulong async_min; /* minimum number of ticks between processing a housekeeping event, positive integer power of 2 */

  /* epoll state */
  struct epoll_event events[ FD_TILE_NET_DGRAM_SOCKET_MAX ];
  int                event_idx = -1;

  /* TX batching */
  struct mmsghdr * tx_batch;
  uint             tx_batch_cnt;
  uint             tx_burst;

  /* RX batching */
  struct mmsghdr * rx_msg;
  uint             rx_burst;
  ulong            rx_slot_idx;    /* next publish at this slot (in [0,rx_slot_wmark]) */
  ulong            rx_slot_wmark;  /* wraparound to 0 when idx crosses this point */

  do {

    FD_LOG_INFO(( "Booting net_dgram_rxtx" ));

    mtu = fd_ulong_align_dn( mtu, FD_CHUNK_ALIGN );  /* below assumes chunk aligned */
    if( FD_UNLIKELY( mtu < HEADROOM ) ) {
      FD_LOG_WARNING(( "invalid headroom" ));
      return 1;
    }

    /* rx frag stream init */

    if( FD_UNLIKELY( !rx_mcache ) ) { FD_LOG_WARNING(( "NULL rx_mcache" )); return 1; }
    rx_depth = fd_mcache_depth    ( rx_mcache );
    rx_sync  = fd_mcache_seq_laddr( rx_mcache );
    rx_seq   = seq0;

    /* scratch init */

    FD_SCRATCH_ALLOC_INIT( scratch, cfg->scratch );
    if( FD_UNLIKELY( fdgen_tile_net_dgram_scratch_footprint( rx_depth, cfg->rx_burst, cfg->tx_burst, mtu )
                     > cfg->scratch_sz ) ) {
      FD_LOG_WARNING(( "undersz scratch region" ));
      return 1;
    }

    /* cnc state init */

    if( FD_UNLIKELY( !cnc ) ) { FD_LOG_WARNING(( "NULL cnc" )); return 1; }
    if( FD_UNLIKELY( fd_cnc_app_sz( cnc )<64UL ) ) { FD_LOG_WARNING(( "undersz cnc diag" )); return 1; }
    if( FD_UNLIKELY( fd_cnc_signal_query( cnc )!=FD_CNC_SIGNAL_BOOT ) ) { FD_LOG_WARNING(( "already booted" )); return 1; }

    cnc_diag = fd_cnc_app_laddr( cnc );

    cnc_diag_backp_cnt   = 0UL;
    cnc_diag_overnp_cnt  = 0UL;
    cnc_diag_tx_pub_cnt  = 0UL;
    cnc_diag_tx_pub_sz   = 0UL;
    cnc_diag_tx_filt_cnt = 0UL;
    cnc_diag_rx_cnt      = 0UL;
    cnc_diag_rx_sz       = 0UL;

    /* tx frag stream init */

    if( FD_UNLIKELY( !tx_mcache ) ) { FD_LOG_WARNING(( "NULL tx_mcache")); return 1; }
    tx_depth = fd_mcache_depth( tx_mcache );
    tx_seq   = fd_mcache_seq_query( fd_mcache_seq_laddr( tx_mcache ) );

    if( FD_UNLIKELY( !tx_base ) ) { FD_LOG_WARNING(( "NULL tx_base" )); return 1; }

    /* tx batch init */

    tx_burst = cfg->tx_burst;
    if( FD_UNLIKELY( !tx_burst ) ) {
      FD_LOG_WARNING(( "zero tx_burst" ));
      return 1;
    }

    tx_batch_cnt = 0U;
    struct sockaddr_storage * tx_addrs;
    struct iovec *            tx_iov;
    uchar *                   tx_buf;
    tx_addrs = FD_SCRATCH_ALLOC_APPEND( scratch, alignof(struct sockaddr_storage), tx_burst*sizeof(struct sockaddr_storage) );
    tx_iov   = FD_SCRATCH_ALLOC_APPEND( scratch, alignof(struct iovec),            tx_burst*sizeof(struct iovec)   );
    tx_batch = FD_SCRATCH_ALLOC_APPEND( scratch, alignof(struct mmsghdr),          tx_burst*sizeof(struct mmsghdr) );
    tx_buf   = FD_SCRATCH_ALLOC_APPEND( scratch, FD_CHUNK_ALIGN,                   tx_burst*mtu                    );
    fd_memset( tx_batch, 0, sizeof(struct mmsghdr)*tx_burst );
    for( ulong j=0UL; j<tx_burst; j++ ) {
      tx_batch[ j ].msg_hdr.msg_name    = tx_addrs + j;
      tx_batch[ j ].msg_hdr.msg_namelen = sizeof(struct sockaddr_storage);
      tx_batch[ j ].msg_hdr.msg_iov     = tx_iov + j;
      tx_batch[ j ].msg_hdr.msg_iovlen  = 1;
      tx_iov  [ j ].iov_base            = tx_buf;
      tx_iov  [ j ].iov_len             = mtu;
      tx_buf += mtu;
    }

    /* rx batch init */

    ulong rx_slot_max;
    rx_burst = cfg->rx_burst;
    if( FD_UNLIKELY( !rx_burst ) ) {
      FD_LOG_WARNING(( "invalid rx_batch_max" ));
      return 1;
    }
    rx_slot_idx   = 0UL;
    rx_slot_wmark = rx_depth +   rx_burst;
    rx_slot_max   = rx_depth + 2*rx_burst;

    if( FD_UNLIKELY( !rx_dcache ) ) { FD_LOG_WARNING(( "NULL dcache" )); return 1; }
    if( FD_UNLIKELY( !rx_base   ) ) { FD_LOG_WARNING(( "NULL base"   )); return 1; }
    if( FD_UNLIKELY( fd_dcache_data_sz( rx_dcache ) < rx_slot_max*mtu ) ) {
      FD_LOG_WARNING(( "undersz dcache (need %lu mtu sz (%lu) slots)", rx_slot_max, mtu ));
      return 1;
    }

    struct sockaddr_storage * rx_addrs;  /* consider using sockaddr_in instead */
    struct iovec *            rx_iov;
    rx_addrs = FD_SCRATCH_ALLOC_APPEND( scratch, alignof(struct sockaddr_storage), rx_slot_max*sizeof(struct sockaddr_storage) );
    rx_iov   = FD_SCRATCH_ALLOC_APPEND( scratch, alignof(struct iovec),            rx_slot_max*sizeof(struct iovec)            );
    rx_msg   = FD_SCRATCH_ALLOC_APPEND( scratch, alignof(struct mmsghdr),          rx_slot_max*sizeof(struct mmsghdr)          );
    fd_memset( rx_msg, 0, rx_slot_max*sizeof(struct mmsghdr) );
    uchar * rx_cur = rx_dcache;
    for( ulong j=0UL; j<rx_slot_max; j++ ) {
      rx_msg[ j ].msg_hdr.msg_name    = rx_addrs + j;
      rx_msg[ j ].msg_hdr.msg_namelen = sizeof(struct sockaddr_storage);
      rx_msg[ j ].msg_hdr.msg_iov     = rx_iov + j;
      rx_msg[ j ].msg_hdr.msg_iovlen  = 1;
      rx_iov[ j ].iov_base            = rx_cur + HEADROOM;
      rx_iov[ j ].iov_len             = mtu    - HEADROOM;
      rx_cur += mtu;
    }

    /* housekeeping init */

    if( lazy<=0L ) lazy = fd_tempo_lazy_default( rx_depth );
    FD_LOG_INFO(( "Configuring housekeeping (lazy %li ns)", lazy ));

    async_min = fd_tempo_async_min( lazy, 1UL /*event_cnt*/, (float)tick_per_ns );
    if( FD_UNLIKELY( !async_min ) ) { FD_LOG_WARNING(( "bad lazy" )); return 1; }

    /* Sanity check that scratch allocations were within bounds */
    assert( _scratch <= (ulong)cfg->scratch + cfg->scratch_sz );

  } while(0);

  FD_LOG_INFO(( "Running datagram socket driver (orig %lu)", orig ));
  fd_cnc_signal( cnc, FD_CNC_SIGNAL_RUN );
  long then = fd_tickcount();
  long now  = then;
  for(;;) {
    now = fd_tickcount();

    /* Do housekeeping at a low rate in the background */

    if( FD_UNLIKELY( (now-then)>=0L || tx_batch_cnt==tx_burst ) ) {
      /* Send synchronization info */
      fd_mcache_seq_update( rx_sync, rx_seq );

      /* Send diagnostic info */
      fd_cnc_heartbeat( cnc, now );
      FD_COMPILER_MFENCE();
      cnc_diag->backp_cnt   += cnc_diag_backp_cnt;
      cnc_diag->tx_pub_cnt  += cnc_diag_tx_pub_cnt;
      cnc_diag->tx_pub_sz   += cnc_diag_tx_pub_sz;
      cnc_diag->tx_filt_cnt += cnc_diag_tx_filt_cnt;
      cnc_diag->rx_cnt      += cnc_diag_rx_cnt;
      cnc_diag->rx_sz       += cnc_diag_rx_sz;
      cnc_diag->overnp_cnt  += cnc_diag_overnp_cnt;
      FD_COMPILER_MFENCE();
      cnc_diag_backp_cnt   = 0UL;
      cnc_diag_tx_pub_cnt  = 0UL;
      cnc_diag_tx_pub_sz   = 0UL;
      cnc_diag_tx_filt_cnt = 0UL;
      cnc_diag_rx_cnt      = 0UL;
      cnc_diag_rx_sz       = 0UL;
      cnc_diag_overnp_cnt  = 0UL;

      /* Receive command-and-control signals */
      ulong s = fd_cnc_signal_query( cnc );
      if( FD_UNLIKELY( s!=FD_CNC_SIGNAL_RUN ) ) {
        if( FD_LIKELY( s==FD_CNC_SIGNAL_HALT ) ) break;
        fd_cnc_signal( cnc, FD_CNC_SIGNAL_RUN );
      }

      /* Reload housekeeping timer */
      then = now + (long)fd_tempo_async_reload( rng, async_min );

      /* Flush TX batch */
      if( tx_batch_cnt ) {
        long send_cnt = sendmmsg( send_fd, tx_batch, tx_batch_cnt, MSG_DONTWAIT );
        if( send_cnt!=(long)tx_batch_cnt ) cnc_diag_backp_cnt++;
        tx_batch_cnt = 0U;
        goto flush_rx; /* Always do RX after TX flush */
      }
    }

    /* Check if there is a new outgoing packet */

    fd_frag_meta_t const * tx_mline = tx_mcache + fd_mcache_line_idx( tx_seq, tx_depth );

    FD_COMPILER_MFENCE();
    __m128i tx_mline_sse0 = _mm_load_si128( &tx_mline->sse0 );
    FD_COMPILER_MFENCE();
    __m128i tx_mline_sse1 = _mm_load_si128( &tx_mline->sse1 );
    FD_COMPILER_MFENCE();

    ulong tx_seq_found = fd_frag_meta_sse0_seq( tx_mline_sse0 );
    long  tx_diff      = fd_seq_diff( tx_seq_found, tx_seq );
    if( FD_UNLIKELY( tx_diff>0L ) ) {
      cnc_diag_overnp_cnt++;
      tx_seq = tx_seq_found;
      continue;
    }

    if( tx_diff==0UL ) {

      /* We have a packet to transmit */
      struct mmsghdr *     hdr     = tx_batch + tx_batch_cnt;
      struct sockaddr_in * saddr4  = fd_type_pun( hdr->msg_hdr.msg_name );
      uchar *              payload = hdr->msg_hdr.msg_iov->iov_base;
      hdr->msg_hdr.msg_namelen     = sizeof(struct sockaddr_in);
      saddr4->sin_family           = AF_INET;

      /* Do speculative reads */
      ulong                sz       = fd_frag_meta_sse1_sz( tx_mline_sse1 );
      uchar const *        frame    = fd_chunk_to_laddr_const( tx_base, fd_frag_meta_sse1_chunk( tx_mline_sse1 ) );
      uchar const *        cur      = frame;
      fd_eth_hdr_t const * eth_hdr  = fd_type_pun_const( cur );  cur += sizeof(fd_eth_hdr_t);
      fd_ip4_hdr_t const * ip4_hdr  = fd_type_pun_const( cur );  cur += FD_IP4_GET_IHL( *ip4_hdr )<<2;
      fd_udp_hdr_t const * udp_hdr  = fd_type_pun_const( cur );  cur += sizeof(fd_udp_hdr_t);
      uchar const *        data     = cur;
      long                 data_sz_ = sz - ((ulong)udp_hdr + 8UL - (ulong)frame);

      /* Stateless verify, impossible with well-behaving producer even
         in case of torn read (reads guaranteed atomic) */
      if( FD_UNLIKELY( ( eth_hdr->net_type != FD_ETH_HDR_TYPE_IP ) |
                       ( FD_IP4_GET_VERSION( *ip4_hdr ) != 4     ) |
                       ( sz > mtu                                ) |
                       ( data_sz_ < 0                            ) ) ) {
        cnc_diag_tx_filt_cnt++;
        tx_seq = fd_seq_inc( tx_seq, 1 );
        continue;
      }

      /* Speculative copy
         FIXME Add support for reliable mode and remove this copy */
      ulong data_sz = hdr->msg_hdr.msg_iov->iov_len = (ulong)data_sz_;
      hdr->msg_len  = (uint)data_sz;
      FD_COMPILER_MFENCE();
      memcpy( &saddr4->sin_addr.s_addr, &ip4_hdr->daddr_c, 4UL );
      saddr4->sin_port = udp_hdr->net_dport;
      fd_memcpy( payload, data, data_sz );
      FD_COMPILER_MFENCE();

      /* Detect overrun
         FIXME this could be moved to batch flush */
      tx_seq_found = fd_frag_meta_seq_query( tx_mline );
      if( FD_UNLIKELY( tx_seq!=tx_seq_found ) ) {
        cnc_diag_overnp_cnt++;
        tx_seq = tx_seq_found;  /* FIXME might jump back */
        continue;
      }

      /* Wind up for the next iteration */
      tx_batch_cnt++;
      tx_seq = fd_seq_inc( tx_seq, 1 );
      continue;

    }

    /* Pull incoming packets */

flush_rx:
    if( event_idx<0 ) {
      int event_cnt = epoll_wait( epoll_fd, events, FD_TILE_NET_DGRAM_SOCKET_MAX, 0 );
      if( event_cnt<0 ) {
        int err = errno;
        if( FD_LIKELY( err==EINTR ) ) continue;
        FD_LOG_WARNING(( "epoll_wait failed (%i-%s)", err, fd_io_strerror( err ) ));
        return 1;
      }
      event_idx = event_cnt-1;
      continue;
    }

    /* RX available, receive packets into dcache */

    fdgen_tile_net_dgram_epoll_data_t user_data;
    user_data.u64 = events[ event_idx ].data.u64;
    struct mmsghdr * rx_batch = rx_msg + rx_slot_idx;

    long rx_batch_cnt_ = recvmmsg( user_data.fd, rx_batch, rx_burst, MSG_DONTWAIT, NULL );
    if( rx_batch_cnt_<0 ) {
      int err = errno;
      if( FD_LIKELY( err==EAGAIN ) ) {
        now = fd_tickcount();
        continue;
      }
      FD_LOG_WARNING(( "recvmmsg failed (%i-%s)", err, fd_io_strerror( err ) ));
      return 1;
    }
    ulong rx_batch_cnt = (ulong)rx_batch_cnt_;

    /* Burst publish and wind up for next receive */

    for( ulong j=0UL; j<rx_batch_cnt; j++ ) {
      /* Packet info */
      struct mmsghdr *     msg      = rx_batch + j;
      ulong                sz       = (ulong)msg->msg_len + HEADROOM;
      ulong                sig      = 0UL;  /* TODO format sig for filtering */
      uchar *              udp_data = msg->msg_hdr.msg_iov->iov_base;
      uchar *              frame    = udp_data - HEADROOM;
      struct sockaddr_in * saddr4   = fd_type_pun( msg->msg_hdr.msg_name );
      if( FD_UNLIKELY( saddr4->sin_family!=AF_INET ) ) {
        FD_LOG_WARNING(( "unexpected address family %d", saddr4->sin_family ));
        continue;
      }

      /* Craft fake packet headers */
      fd_eth_hdr_t * eth_hdr = fd_type_pun( frame    );
      fd_ip4_hdr_t * ip4_hdr = fd_type_pun( frame+14 );
      fd_udp_hdr_t * udp_hdr = fd_type_pun( frame+54 );
      eth_hdr->net_type = FD_ETH_HDR_TYPE_IP;
      ip4_hdr[0] = (fd_ip4_hdr_t) {
        .verihl       = FD_IP4_VERIHL( 4, 10 ),  /* padded up to IPv6 header size */
        .net_tot_len  = (ushort)fd_ushort_bswap( (ushort)sz ),
        .net_frag_off = (ushort)fd_ushort_bswap( FD_IP4_HDR_FRAG_OFF_DF ),
        .ttl          = 1,
        .protocol     = FD_IP4_HDR_PROTOCOL_UDP
      };
      udp_hdr[0] = (fd_udp_hdr_t) {
        .net_sport = (ushort)fd_ushort_bswap( (ushort)saddr4->sin_port ),
        .net_dport = (ushort)fd_ushort_bswap( (ushort)user_data.dport ),
        .net_len   = sizeof(fd_udp_hdr_t),
        .check     = 0
      };

      /* Publish to fd_tango */
      ulong chunk  = fd_laddr_to_chunk( rx_base, frame );
      ulong ctl    = fd_frag_meta_ctl( orig, 1 /*som*/, 1 /*eom*/, 0 /*err*/ );
      ulong tsorig = fd_frag_meta_ts_comp( now );
      ulong tspub  = tsorig;
      fd_mcache_publish( rx_mcache, rx_depth, rx_seq, sig, chunk, sz, ctl, tsorig, tspub );
      rx_seq = fd_seq_inc( rx_seq, 1UL );

      /* Reset msghdr fields */
      msg->msg_hdr.msg_iov->iov_len = msg->msg_len;  /* FIXME unnecessary? */
      msg->msg_hdr.msg_namelen      = sizeof(struct sockaddr_storage);
    }
    rx_slot_idx += rx_batch_cnt;
    if( rx_slot_idx>rx_slot_wmark ) rx_slot_idx = 0UL; /* cmov */
    event_idx--;

  }

  do {

    FD_LOG_INFO(( "Halted net_dgram_rxtx" ));
    fd_cnc_signal( cnc, FD_CNC_SIGNAL_BOOT );

  } while(0);

  return 0;
}
