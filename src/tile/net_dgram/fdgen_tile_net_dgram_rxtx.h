#pragma once

/* The net_dgram_rxtx tile forwards UDP datagrams been AF_INET sockets
   and fd_tango. */

#include <firedancer/tango/cnc/fd_cnc.h>

/* FD_TILE_NET_DGRAM_SOCKET_MAX is the max number of file descriptors
   that may be registered with the epoll fd. */

#define FD_TILE_NET_DGRAM_SOCKET_MAX (64)

struct fdgen_tile_net_dgram_rxtx_cfg {

  fd_cnc_t *       cnc;
  fd_frag_meta_t * mcache;
  uchar *          dcache;
  uchar *          base;
  fd_rng_t *       rng;

  long tx_flush_timeout;

  int epoll_fd;

};

typedef struct fdgen_tile_net_dgram_rxtx_cfg fdgen_tile_net_dgram_rxtx_cfg_t;

FD_PROTOTYPES_BEGIN

int
fdgen_tile_net_dgram_rxtx_run( fdgen_tile_net_dgram_rxtx_cfg_t * cfg );

FD_PROTOTYPES_BEGIN
