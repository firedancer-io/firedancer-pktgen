#pragma once

/* The net_xsk_poll tile busy polls an AF_XDP kernel driver via the
   poll(2) syscall.

   The poll syscall instructs the kernel to replenish the RX and TX
   AF_XDP rings.  This tile is helpful when working with net_xsk_{rx,tx
   which do not yield to the kernel by themselves. */

#include <firedancer/tango/cnc/fd_cnc.h>

struct fdgen_tile_net_xsk_poll_cfg {
  fd_cnc_t * cnc;
  fd_rng_t * rng;
  long       lazy;
  double     tick_per_ns;
  int        xsk_fd;
};

typedef struct fdgen_tile_net_xsk_poll_cfg fdgen_tile_net_xsk_poll_cfg_t;

FD_PROTOTYPES_BEGIN

int
fdgen_tile_net_xsk_poll_run( fdgen_tile_net_xsk_poll_cfg_t * cfg );

FD_PROTOTYPES_END
