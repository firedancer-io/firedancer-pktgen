#pragma once

/* fdgen_xsk.h is fdgen's spin on fd_xsk.  This version provides more
   flexible buffer management. */

#include <firedancer/tango/fd_tango_base.h>

/* FDGEN_XSK_FRAME_SZ is the size of each AF_XDP frame used throughout
   fdgen.  DO NOT EDIT. */

#define FDGEN_XSK_FRAME_SZ (2048UL)
#define FDGEN_LG_XSK_FRAME_SZ (11)

/* fdgen_xsk_ring_t describes an AF_XDP ring.
   There are four rings: FILL, RX, TX, COMPLETION. */

struct fdgen_xsk_ring {

  /* The kernel-owned memory region holding the AF_XDP ring is mapped
     in local address space at [mem,mem+map_sz)  We remember this range
     in case we need to munmap(2) it later.  */

  void *  mem;
  ulong   map_sz;

  /* Pointers to ring elements.  Beware of cache line bouncing in case
     the kernel thread handling driver IRQs and the tile's thread are on
     different CPUs. */

  union {
    void *            ptr;         /* Opaque pointer */
    struct xdp_desc * packet_ring; /* For RX, TX rings */
    ulong *           frame_ring;  /* For FILL, COMPLETION rings */
  };

  uint * flags;
  uint * prod;
  uint * cons;
  uint   depth;

};

typedef struct fdgen_xsk_ring fdgen_xsk_ring_t;

FD_PROTOTYPES_BEGIN

/* Address translation between XSK UMEM and local */

FD_FN_CONST static inline ulong
fd_laddr_to_umem( void * umem_base,
                  void * laddr ) {
  return (ulong)laddr - (ulong)umem_base;
}

FD_FN_CONST static inline void *
fd_umem_to_laddr( void * umem_base,
                  ulong  umem_off ) {
  return (void *)( umem_base + umem_off );
}

/* Address translation between fd_tango and XSK UMEM */

FD_FN_CONST static inline ulong
fd_chunk_to_umem( void * chunk0,
                  void * umem_base,
                  ulong  chunk ) {
  return fd_laddr_to_umem( umem_base, fd_chunk_to_laddr( chunk0, chunk ) );
}

FD_FN_CONST static inline ulong
fd_umem_to_chunk( void * chunk0,
                  void * umem_base,
                  ulong  umem_off ) {
  return fd_laddr_to_chunk( chunk0, fd_umem_to_laddr( umem_base, umem_off ) );
}

FD_PROTOTYPES_END
