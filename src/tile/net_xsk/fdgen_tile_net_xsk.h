#pragma once

#include <firedancer/util/fd_util_base.h>
#include <immintrin.h>

/* fdgen_tile_net_xsk.h contains common XSK tile definitions */

struct fdgen_tile_net_xsk_rx_diag {
  ulong in_backp;
  ulong backp_cnt;
  ulong pub_cnt;
  ulong pub_sz;
  uint  fr_cons;  /* TODO make this atomic __m128i */
  uint  fr_prod;
  uint  rx_cons;
  uint  rx_prod;
};

typedef struct fdgen_tile_net_xsk_rx_diag fdgen_tile_net_xsk_rx_diag_t;
