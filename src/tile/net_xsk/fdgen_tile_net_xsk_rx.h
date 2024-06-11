#pragma once

/* The net_xsk_rx tile forwards incoming AF_XDP packets to an mcache.

   The kernel-facing side of this tile provides fragment buffers to the
   AF_XDP socket (via an fd_dcache).  It does not wake up the kernel
   (relies on external polling or IRQs).

   Incoming fragments are passed on to downstream consumers via fd_tango
   messages (fd_frag_meta_t) without copying the payload.  No special
   configuration is required at the consumer side.

   Does not yet support fragmentation (AF_XDP multi-buffer) */

#include <firedancer/util/fd_util_base.h>
#include <firedancer/tango/fd_tango_base.h>
#include <firedancer/tango/cnc/fd_cnc.h>
#include "../../xdp/fdgen_xsk.h"

/* fdgen_tile_net_xsk_rx_cfg_t holds config and local joins required by
   the net_xsk_rx tile. */

struct fdgen_tile_net_xsk_rx_cfg {

  ulong            orig;
  long             lazy;
  double           tick_per_ns;
  ulong            seq0;       /* first seq to produce */
  ulong            xsk_burst;  /* frags to burst before updating xsk counters */
  ulong            mtu;

  fd_cnc_t *       cnc;
  fd_frag_meta_t * mcache;     /* xsk_rx -> downstream frags */
  uchar *          dcache;     /* shared frag buffer */
  uchar *          base;       /* frag base pointer */
  fd_rng_t *       rng;

  fdgen_xsk_ring_t ring_fr;    /* xsk_rx -> kernel frag buffers */
  fdgen_xsk_ring_t ring_rx;    /* kernel -> kernel frags */
  uchar *          umem_base;

};

typedef struct fdgen_tile_net_xsk_rx_cfg fdgen_tile_net_xsk_rx_cfg_t;

FD_PROTOTYPES_BEGIN

int
fdgen_tile_net_xsk_rx_run( fdgen_tile_net_xsk_rx_cfg_t * cfg );

FD_PROTOTYPES_END
