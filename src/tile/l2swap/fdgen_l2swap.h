#include <firedancer/tango/cnc/fd_cnc.h>

struct fdgen_tile_l2swap_cfg {
  fd_cnc_t *       in_cnc;
  fd_frag_meta_t * in_mcache;
  uchar *          in_dcache;
  fd_frag_meta_t * out_mcache;
  uchar *          out_dcache;
};

typedef struct fdgen_tile_l2swap_cfg fdgen_tile_l2swap_cfg_t;
