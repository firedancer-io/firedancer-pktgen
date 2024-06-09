#pragma once

/* fdgen_xsk.h is fdgen's spin on fd_xsk.  This version provides more
   flexible buffer management. */

#include <firedancer/util/fd_util_base.h>

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
