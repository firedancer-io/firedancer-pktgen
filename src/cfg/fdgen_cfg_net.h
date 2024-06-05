#pragma once

#include <firedancer/util/fd_util_base.h>

#define FDGEN_NET_MODE_XDP    (1)
#define FDGEN_NET_MODE_SOCKET (2)

/* fdgen_port_range_t defines a port range (inclusive) */

struct fdgen_port_range {
  ushort lo;
  ushort hi;
};

typedef struct fdgen_port_range fdgen_port_range_t;

FD_PROTOTYPES_BEGIN

int
fdgen_cstr_to_net_mode( char const * cstr );

fdgen_port_range_t *
fdgen_cstr_to_port_range( fdgen_port_range_t * ports,
                          char *               cstr );

static inline ulong
fdgen_port_cnt( fdgen_port_range_t const * ports ) {
  if( FD_UNLIKELY( ports->lo > ports->hi ) ) return 0UL;
  return ports->hi - ports->lo;
}

FD_PROTOTYPES_END
