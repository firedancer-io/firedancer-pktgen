#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <firedancer/util/fd_util.h>
#include <firedancer/tango/dcache/fd_dcache.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct fdgen_tile_net_dgram_diag {
  ulong backp_cnt;
  ulong tx_pub_cnt;
  ulong tx_pub_sz;
  ulong tx_filt_cnt;
  ulong rx_cnt;
  ulong rx_sz;
  ulong overnp_cnt;
};

typedef struct fdgen_tile_net_dgram_diag fdgen_tile_net_dgram_diag_t;

FD_PROTOTYPES_BEGIN

/* fdgen_tile_net_dgram_scratch_{align,footprint} specify parameters of
   the scratch memory region for a given configuration. */

FD_FN_CONST static inline ulong
fdgen_tile_net_dgram_scratch_align( void ) {
  return 128UL;  /* arbitrarily large */
}

FD_FN_CONST static inline ulong
fdgen_tile_net_dgram_scratch_footprint( ulong rx_depth,
                                        ulong rx_burst,
                                        ulong tx_burst,
                                        ulong mtu ) {
  ulong rx_slot_max = rx_depth + 2*rx_burst;
  ulong l = FD_LAYOUT_INIT;
  l = FD_LAYOUT_APPEND( l, alignof(struct sockaddr_storage), tx_burst    *sizeof(struct sockaddr_storage) );
  l = FD_LAYOUT_APPEND( l, alignof(struct iovec),            tx_burst    *sizeof(struct iovec)            );
  l = FD_LAYOUT_APPEND( l, alignof(struct mmsghdr),          tx_burst    *sizeof(struct mmsghdr)          );
  l = FD_LAYOUT_APPEND( l, FD_CHUNK_ALIGN,                   tx_burst    *mtu                             );
  l = FD_LAYOUT_APPEND( l, alignof(struct sockaddr_storage), rx_slot_max *sizeof(struct sockaddr_storage) );
  l = FD_LAYOUT_APPEND( l, alignof(struct iovec),            rx_slot_max *sizeof(struct iovec)            );
  l = FD_LAYOUT_APPEND( l, alignof(struct mmsghdr),          rx_slot_max *sizeof(struct mmsghdr)          );
  return FD_LAYOUT_FINI( l, 128UL );
}

/* fdgen_tile_net_dgram_dcache_data_sz returns the dcache footprint
   required for a given configuration. */

FD_FN_CONST static inline ulong
fdgen_tile_net_dgram_dcache_data_sz( ulong rx_depth,
                                     ulong rx_burst,
                                     ulong mtu ) {
  /* FIXME handle overflow */
  ulong rx_slot_max = rx_depth + (2*rx_burst);
  return rx_slot_max * mtu;
}

FD_PROTOTYPES_END
