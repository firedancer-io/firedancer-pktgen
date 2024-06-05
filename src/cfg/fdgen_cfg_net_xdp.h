#pragma once

/* fdgen_cfg_net_xdp.h provides APIs for setting up XDP port redirecting
   which tiles can later bind their XSK queues to. */

#include "fdgen_cfg_net.h"

/* fdgen_ports_xdp owns file descriptors that make up the XDP traffic
   hijacking setup. */

struct fdgen_ports_xdp {
  int xsk_map_fd;
  int prog_fd;
  int link_fd;
};

typedef struct fdgen_ports_xdp fdgen_ports_xdp_t;

FD_PROTOTYPES_BEGIN

fdgen_ports_xdp_t *
fdgen_ports_xdp_init( fdgen_ports_xdp_t * xdp,
                      ulong               xsk_max,
                      uint                ip4,
                      fdgen_port_range_t  port_range,
                      uint                if_idx,
                      uint                if_flags );

void
fdgen_ports_xdp_fini( fdgen_ports_xdp_t * xdp );

FD_PROTOTYPES_END
