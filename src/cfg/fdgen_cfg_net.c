#include "fdgen_cfg_net.h"
#include <firedancer/util/fd_util.h>

int
fdgen_cstr_to_net_mode( char const * cstr ) {
  if( 0==strcmp( cstr, "xdp"    ) ) return FDGEN_NET_MODE_XDP;
  if( 0==strcmp( cstr, "socket" ) ) return FDGEN_NET_MODE_SOCKET;
  return 0;
}

fdgen_port_range_t *
fdgen_cstr_to_port_range( fdgen_port_range_t * ports,
                          char *               cstr ) {

  char * tok[2];
  ulong tok_cnt = fd_cstr_tokenize( tok, 2, cstr, '-' );

  ulong lo; ulong hi;

  if( tok_cnt==2 ) {
    lo = fd_cstr_to_ulong( tok[0] );
    hi = fd_cstr_to_ulong( tok[1] );
  } else if( tok_cnt==1 ) {
    lo = fd_cstr_to_ulong( tok[0] );
    hi = lo + 1UL;
  } else {
    return NULL;
  }

  if( lo==0 || hi==0 ) return NULL;
  if( lo>hi          ) return NULL;
  if( lo>USHORT_MAX  ) return NULL;
  if( hi>USHORT_MAX  ) return NULL;

  ports->lo = (ushort)lo;
  ports->hi = (ushort)hi;

  return ports;
}