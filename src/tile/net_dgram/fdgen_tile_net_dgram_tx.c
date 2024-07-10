#define _GNU_SOURCE
#include "fdgen_tile_net_dgram_tx.h"
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

/* Transmit Side *******************************************************

   TODO */

#define HEADROOM (42UL)  /* Ethernet header, IPv4 header, UDP header */

int
fdgen_tile_net_dgram_tx_run( fdgen_tile_net_dgram_tx_cfg_t * cfg ) {

  if( FD_UNLIKELY( !cfg ) ) { FD_LOG_WARNING(( "NULL cfg" )); return 1; }

  /* load config */

  ulong            orig        = cfg->orig;
  long             lazy        = cfg->lazy;
  double           tick_per_ns = cfg->tick_per_ns;
  ulong            mtu         = cfg->mtu;
  fd_cnc_t *       cnc         = cfg->cnc;
  fd_rng_t *       rng         = cfg->rng;
  fd_frag_meta_t * tx_mcache   = cfg->tx_mcache;
  uchar *          tx_base     = cfg->tx_base;
  int              send_fd     = cfg->send_fd;

  /* cnc state */
  fdgen_tile_net_dgram_diag_t * cnc_diag;
  ulong   cnc_diag_backp_cnt;
  ulong   cnc_diag_tx_pub_cnt;
  ulong   cnc_diag_tx_pub_sz;
  ulong   cnc_diag_tx_filt_cnt;
  ulong   cnc_diag_overnp_cnt;

  /* tx (in) frag stream state */
  ulong   tx_depth;
  ulong   tx_seq;

  /* housekeeping state */
  ulong async_min; /* minimum number of ticks between processing a housekeeping event, positive integer power of 2 */

  /* TX batching */
  struct mmsghdr * tx_batch;
  uint             tx_batch_cnt;
  uint             tx_burst;

  do {

    FD_LOG_INFO(( "Booting net_dgram_tx" ));

    mtu = fd_ulong_align_dn( mtu, FD_CHUNK_ALIGN );  /* below assumes chunk aligned */
    if( FD_UNLIKELY( mtu < HEADROOM ) ) {
      FD_LOG_WARNING(( "invalid headroom" ));
      return 1;
    }

    /* scratch init */

    FD_SCRATCH_ALLOC_INIT( scratch, cfg->scratch );
    if( FD_UNLIKELY( fdgen_tile_net_dgram_scratch_footprint( 0, 0, cfg->tx_burst, mtu )
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

    /* housekeeping init */

    if( lazy<=0L ) lazy = fd_tempo_lazy_default( tx_depth );
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
      /* Send diagnostic info */
      fd_cnc_heartbeat( cnc, now );
      FD_COMPILER_MFENCE();
      cnc_diag->backp_cnt   += cnc_diag_backp_cnt;
      cnc_diag->tx_pub_cnt  += cnc_diag_tx_pub_cnt;
      cnc_diag->tx_pub_sz   += cnc_diag_tx_pub_sz;
      cnc_diag->tx_filt_cnt += cnc_diag_tx_filt_cnt;
      cnc_diag->overnp_cnt  += cnc_diag_overnp_cnt;
      FD_COMPILER_MFENCE();
      cnc_diag_backp_cnt   = 0UL;
      cnc_diag_tx_pub_cnt  = 0UL;
      cnc_diag_tx_pub_sz   = 0UL;
      cnc_diag_tx_filt_cnt = 0UL;
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
        continue;
      }
    }

    /* Check if there is a new outgoing packet
       FIXME use FD_MCACHE_WAIT_REG here */

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

    if( tx_diff!=0UL ) {
      FD_SPIN_PAUSE();
      continue;
    }

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

  do {

    FD_LOG_INFO(( "Halted net_dgram_tx" ));
    fd_cnc_signal( cnc, FD_CNC_SIGNAL_BOOT );

  } while(0);

  return 0;
}
