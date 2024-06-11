#pragma once

#include <firedancer/util/fd_util_base.h>

FD_PROTOTYPES_BEGIN

/* fdgen_netlink_connect creates a well-configured AF_NETLINK socket
   with a given NETLINK_{...} mode param.  Returns the socket FD on
   success. On failure, returns -1 and logs errno to warning log. */

int
fdgen_netlink_connect( int mode );

/* fdgen_create_veth_pair creates a veth interface pair.  Attempts to
   create an interface with given name that appears in network
   namespaces at file descriptors netns1 and netns2.  (To acquiere these
   FDs, use open(/proc/pid/ns/net)).  netlink is a socket of type
   NETLINK_ROUTE.  Returns 0 on success. On failure, logs error details
   to warning log and returns -1. */

struct fdgen_veth_params {
  char const * name;
  uchar        name_sz;  /* including NUL */
  int          netns;
  uchar        mac_addr[6];
  uint         rx_queue_cnt;
  uint         tx_queue_cnt;
};

typedef struct fdgen_veth_params fdgen_veth_params_t;

int
fdgen_netlink_create_veth_pair( int                 netlink,
                                fdgen_veth_params_t params[2] );

/* fdgen_create_veth_env is a convenience function to set up two network
   namespaces with a veth each.  env->{rx,tx}_queue_count specify the
   queue counts of the veth devices.  env->params is populated with
   auto-generated settings (including dev name, netns file descriptor,
   MAC address, IP address).  Aborts the process on failure.  Causes a
   brief interruption in network connectivity while switching netns.
   Requires CAP_SYS_ADMIN. */

struct fdgen_veth_env {
  uint rx_queue_cnt[2];  /* in */
  uint tx_queue_cnt[2];  /* in */

  fdgen_veth_params_t params[2];   /* out */
  uint                if_idx[2];   /* out */
  uint                ip_addr[2];  /* out */
};

typedef struct fdgen_veth_env fdgen_veth_env_t;

void
fdgen_netlink_create_veth_env( fdgen_veth_env_t * env );

FD_PROTOTYPES_END
