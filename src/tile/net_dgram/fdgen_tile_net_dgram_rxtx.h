#pragma once

/* The net_dgram_rxtx tile forwards UDP datagrams been AF_INET sockets
   and fd_tango.

   # RX multiplex

   This tile receives from multiple sockets simultaneously via epoll.
   Resolving event metadata is done via fdgen_tile_net_dgram_epoll_data_t.

   # RX flow

   Received packets are published onto the rx_{mcache,dcache} pair.
   Each packet contains a 62 byte dummy header encoding IP/UDP src/dst.
   The IPv4 header at offset 14 is extended to 40 bytes (IHL 10).
   rx_mache is in unreliable mode, i.e. it does not respect consumer
   flow control credits and consumers may be overrun while reading.

   # TX flow

   TODO

   The IPv4 and UDP length fields are ignored. */

#include <firedancer/tango/cnc/fd_cnc.h>
#include <stdint.h>  /* uint64_t */

/* FD_TILE_NET_DGRAM_SOCKET_MAX is the max number of file descriptors
   that may be registered with the epoll fd. */

#define FD_TILE_NET_DGRAM_SOCKET_MAX (64)

/* epoll user data is expected to be fdgen_tile_net_dgram_epoll_data_t */

union fdgen_tile_net_dgram_epoll_data {
  uint64_t u64;
  struct {
    int    fd;
    ushort dport;
  };
};

typedef union fdgen_tile_net_dgram_epoll_data fdgen_tile_net_dgram_epoll_data_t;

struct fdgen_tile_net_dgram_rxtx_diag {
  ulong backp_cnt;
  ulong rx_cnt;
  ulong rx_sz;
  ulong tx_pub_cnt;
  ulong tx_pub_sz;
  ulong tx_filt_cnt;
  ulong overnp_cnt;
};

typedef struct fdgen_tile_net_dgram_rxtx_diag fdgen_tile_net_dgram_rxtx_diag_t;

struct fdgen_tile_net_dgram_rxtx_cfg {

  ulong  orig;
  long   lazy;
  double tick_per_ns;
  ulong  seq0;        /* first seq to produce */
  ulong  mtu;

  fd_rng_t *       rng;
  fd_cnc_t *       cnc;
  uchar *          tx_base;
  fd_frag_meta_t * tx_mcache;
  uchar *          rx_base;
  fd_frag_meta_t * rx_mcache;
  uchar *          rx_dcache;

  ulong tx_burst;          /* sendmmsg batch limit */
  long  tx_burst_timeout;  /* sendmmsg flush timeout (ticks) */
  ulong rx_burst;          /* recvmmsg batch lmit */

  int epoll_fd;  /* level-triggered epoll with fdgen_tile_net_dgram_epoll_data_t user datas */
  int send_fd;   /* unbound AF_INET SOCK_DGRAM socket */

  uchar * scratch;
  ulong   scratch_sz;

};

typedef struct fdgen_tile_net_dgram_rxtx_cfg fdgen_tile_net_dgram_rxtx_cfg_t;

FD_PROTOTYPES_BEGIN

/* fdgen_tile_net_dgram_scratch_{align,footprint} specify parameters of
   the scratch memory region for a given configuration. */

FD_FN_CONST ulong
fdgen_tile_net_dgram_scratch_align( void );

FD_FN_CONST ulong
fdgen_tile_net_dgram_scratch_footprint( ulong rx_depth,
                                        ulong rx_burst,
                                        ulong tx_burst,
                                        ulong mtu );

/* fdgen_tile_net_dgram_dcache_data_sz returns the dcache footprint
   required for a given configuration. */

FD_FN_CONST ulong
fdgen_tile_net_dgram_dcache_data_sz( ulong rx_depth,
                                     ulong rx_burst,
                                     ulong mtu );

/* fdgen_tile_net_dgram_rxtx_run enters the tile main loop. */

int
fdgen_tile_net_dgram_rxtx_run( fdgen_tile_net_dgram_rxtx_cfg_t * cfg );

FD_PROTOTYPES_BEGIN
