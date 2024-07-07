#pragma once
#include <firedancer/tango/cnc/fd_cnc.h>

struct fdgen_tile_udp_flood_cfg {
  fd_cnc_t *       cnc;
  fd_frag_meta_t * mcache;
  uchar *          dcache;
  fd_rng_t *       rng;
  ulong *          out_fseq;
  ulong *          out_fseq_diag_slow_cnt;
  ulong            orig;
  long             lazy;
  ulong            cr_max;

  uint   src_ip;
  uint   dst_ip;
  ushort src_port;
  ushort dst_port;
  uchar  src_mac[6];
  uchar  dst_mac[6];
};

typedef struct fdgen_tile_udp_flood_cfg fdgen_tile_udp_flood_cfg_t;

struct fdgen_tile_udp_flood_diag {
  ulong in_backp;
  ulong backp_cnt;
  ulong pub_cnt;
  ulong pub_sz;
  ulong chunk_idx;
};

typedef struct fdgen_tile_udp_flood_diag fdgen_tile_udp_flood_diag_t;

FD_PROTOTYPES_BEGIN

int
fdgen_tile_udp_flood_run( fdgen_tile_udp_flood_cfg_t * cfg );

FD_PROTOTYPES_END
