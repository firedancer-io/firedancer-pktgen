#pragma once

/* The net_dgram_tx tile forwards UDP datagrams from fd_tango to an
   unbound AF_INET SOCK_DGRAM socket.

   TODO

   The IPv4 and UDP length fields are ignored. */

#include <firedancer/tango/cnc/fd_cnc.h>
#include <stdint.h>  /* uint64_t */

struct fdgen_tile_net_dgram_tx_cfg {

  ulong  orig;
  long   lazy;
  double tick_per_ns;
  ulong  mtu;

  fd_rng_t *       rng;
  fd_cnc_t *       cnc;
  uchar *          tx_base;
  fd_frag_meta_t * tx_mcache;

  ulong tx_burst;          /* sendmmsg batch limit */
  long  tx_burst_timeout;  /* sendmmsg flush timeout (ticks) */

  int send_fd;   /* unbound AF_INET SOCK_DGRAM socket */

  uchar * scratch;
  ulong   scratch_sz;

};

typedef struct fdgen_tile_net_dgram_tx_cfg fdgen_tile_net_dgram_tx_cfg_t;

FD_PROTOTYPES_BEGIN

/* fdgen_tile_net_dgram_tx_run enters the tile main loop. */

int
fdgen_tile_net_dgram_tx_run( fdgen_tile_net_dgram_tx_cfg_t * cfg );

FD_PROTOTYPES_BEGIN
