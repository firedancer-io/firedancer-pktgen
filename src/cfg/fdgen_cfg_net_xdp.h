#pragma once

/* fdgen_cfg_net_xdp.h provides APIs for setting up XDP -> AF_XDP
   redirection. */

#include "fdgen_cfg_net.h"

/* fdgen_xdp_port_redir_t manages a port hijacking setup.  It redirects
   a range of UDP ports for a given dest IPv4 address to AF_XDP. */

struct fdgen_xdp_port_redir {
  int xsk_map_fd;
  int prog_fd;
  int link_fd;
};

typedef struct fdgen_xdp_port_redir fdgen_xdp_port_redir_t;

FD_PROTOTYPES_BEGIN

fdgen_xdp_port_redir_t *
fdgen_xdp_port_redir_init( fdgen_xdp_port_redir_t * redir,
                           ulong                    xsk_max,
                           uint                     ip4,
                           fdgen_port_range_t       port_range,
                           uint                     if_idx,
                           uint                     if_flags );

void
fdgen_xdp_port_redir_fini( fdgen_xdp_port_redir_t * xdp );

FD_PROTOTYPES_END
