#include "fdgen_cfg_net_xdp.h"

#include <assert.h>
#include <errno.h>
#include <linux/bpf.h>
#include <linux/if_xdp.h>
#include <linux/if_link.h>

#include <firedancer/waltz/ebpf/fd_ebpf.h>
#include <firedancer/util/fd_util.h>
#include <firedancer/util/net/fd_eth.h>
#include <firedancer/util/net/fd_ip4.h>
#include <firedancer/util/net/fd_udp.h>

#ifndef BPF_LINK_CREATE
#define BPF_LINK_CREATE (28)
#endif

#ifndef BPF_XDP
#define BPF_XDP (37)
#endif

struct __attribute__((aligned(8))) bpf_link_create {
  uint prog_fd;
  uint target_ifindex;
  uint attach_type;
  uint flags;
};

extern uchar const _binary_fdgen_xdp_ports_o_start[];
extern uchar       _binary_fdgen_xdp_ports_o_size;

fdgen_ports_xdp_t *
fdgen_ports_xdp_init( fdgen_ports_xdp_t * xdp,
                      ulong               xsk_max,
                      uint                ip4,
                      fdgen_port_range_t  port_range,
                      uint                if_idx,
                      uint                if_flags ) {

  memset( xdp, 0, sizeof(fdgen_ports_xdp_t) );

  union bpf_attr attr = {
    .map_type    = BPF_MAP_TYPE_XSKMAP,
    .key_size    = 4U,
    .value_size  = 4U,
    .max_entries = xsk_max,
    .map_name    = "fd_xdp_xsks"
  };
  int xsks_fd = (int)bpf( BPF_MAP_CREATE, &attr, sizeof(union bpf_attr) );
  if( FD_UNLIKELY( xsks_fd<0 ) ) {
    FD_LOG_WARNING(( "bpf(BPF_MAP_CREATE) failed (%i-%s)", errno, fd_io_strerror( errno ) ));
    return NULL;
  }
  xdp->xsk_map_fd = xsks_fd;

  /* Link BPF bytecode */

  ulong elf_sz = (ulong)&_binary_fdgen_xdp_ports_o_size;
  uchar elf_copy[ 1384 ];
  assert( elf_sz <= sizeof(elf_copy) );
  fd_memcpy( elf_copy, _binary_fdgen_xdp_ports_o_start, 1384 );

  fd_ebpf_sym_t syms[ 4 ] = {
    { .name = "fd_xdp_xsks",    .value = xsks_fd       },
    { .name = "fd_xdp_ip",      .value = ip4           },
    { .name = "fd_xdp_port_lo", .value = port_range.lo },
    { .name = "fd_xdp_port_hi", .value = port_range.hi }
  };
  fd_ebpf_link_opts_t opts = {
    .section  = "xdp",
    .sym      = syms,
    .sym_cnt  = 4
  };
  fd_ebpf_link_opts_t * res = fd_ebpf_static_link( &opts, elf_copy, elf_sz );

  if( FD_UNLIKELY( !res ) ) {
    FD_LOG_WARNING(( "Failed to link eBPF bytecode" ));
    fdgen_ports_xdp_fini( xdp );
    return NULL;
  }

  /* Load eBPF program into kernel */

  #define EBPF_KERN_LOG_BUFSZ (32768UL)
  static FD_TL char ebpf_kern_log[ EBPF_KERN_LOG_BUFSZ ];

  attr = (union bpf_attr) {
    .prog_type = BPF_PROG_TYPE_XDP,
    .insn_cnt  = (uint) ( res->bpf_sz / 8UL ),
    .insns     = (ulong)( res->bpf ),
    .license   = (ulong)"Apache-2.0",
    /* Verifier logs */
    .log_level = 6,
    .log_size  = EBPF_KERN_LOG_BUFSZ,
    .log_buf   = (ulong)ebpf_kern_log
  };
  int prog_fd = (int)bpf( BPF_PROG_LOAD, &attr, sizeof(union bpf_attr) );
  if( FD_UNLIKELY( prog_fd<0 ) ) {
    FD_LOG_WARNING(( "bpf(BPF_PROG_LOAD, insns=%p, insn_cnt=%lu) failed (%i-%s)",
                     (void *)res->bpf, res->bpf_sz / 8UL, errno, fd_io_strerror( errno ) ));
    FD_LOG_NOTICE(( "eBPF verifier log:\n%s", ebpf_kern_log ));
    fdgen_ports_xdp_fini( xdp );
    return NULL;
  }
  xdp->prog_fd = prog_fd;

  /* Install program to device */

  struct bpf_link_create link_create = {
    .prog_fd        = (uint)prog_fd,
    .target_ifindex = if_idx,
    .attach_type    = BPF_XDP,
    .flags          = if_flags
  };

  int link_fd = (int)bpf( BPF_LINK_CREATE, fd_type_pun( &link_create ), sizeof(struct bpf_link_create) );
  if( FD_UNLIKELY( link_fd<0 ) ) {
    FD_LOG_WARNING(( "BPF_LINK_CREATE failed (%i-%s)", errno, fd_io_strerror( errno ) ));
    fdgen_ports_xdp_fini( xdp );
    return NULL;
  }

  return xdp;
}

void
fdgen_ports_xdp_fini( fdgen_ports_xdp_t * xdp ) {

  if( FD_UNLIKELY( !xdp ) ) return;

  if( xdp->link_fd >= 0 ) {
    close( xdp->link_fd );
    xdp->link_fd = -1;
  }

  if( xdp->prog_fd >= 0 ) {
    close( xdp->prog_fd );
    xdp->prog_fd = -1;
  }

  if( xdp->xsk_map_fd >= 0 ) {
    close( xdp->xsk_map_fd );
    xdp->xsk_map_fd = -1;
  }
}
