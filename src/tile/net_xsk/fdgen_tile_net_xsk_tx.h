#pragma once

#include <firedancer/util/fd_util_base.h>
#include <firedancer/tango/cnc/fd_cnc.h>
#include "../../xdp/fdgen_xsk.h"

struct fdgen_tile_net_xsk_tx_cfg {

  ulong            orig;
  long             lazy;
  double           tick_per_ns;
  ulong            seq0;       /* first seq to produce */
  ulong            xsk_burst;  /* frags to burst before updating xsk counters */
  ulong            mtu;

  fd_cnc_t *       cnc;
  fd_frag_meta_t * mcache;     /* upstream -> xsk_tx frags */
  uchar *          dcache;     /* shared frag buffer */
  uchar *          base;       /* frag base pointer */
  fd_rng_t *       rng;

  fdgen_xsk_ring_t ring_tx;    /* xsk_tx -> kernel frags */
  fdgen_xsk_ring_t ring_cr;    /* kernel -> xsk_tx frag buffers */
  uchar *          umem_base;
  uchar *          frame0;

};

typedef struct fdgen_tile_net_xsk_tx_cfg fdgen_tile_net_xsk_tx_cfg_t;
