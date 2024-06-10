#pragma once

#include <firedancer/util/fd_util_base.h>

FD_PROTOTYPES_BEGIN

/* fdgen_create_netns creates a new anonymous network namespace. Uses
   unshare(CLONE_NEWNET) to create a new network namespace.  Then,
   immediately uses setns(CLONE_NEWNET) to revert to the original netns.
   This results in a brief interruption of connectivity.  Returns the
   file descriptor on success.  On failure, logs errno and returns -1.
   Requires CAP_SYS_ADMIN. */

int
fdgen_create_netns( void );

/* fdgen_netlink_connect creates a well-configured AF_NETLINK socket
   with a given NETLINK_{...} mode param.  Returns the socket FD on
   success. On failure, returns -1 and logs errno to warning log. */

int
fdgen_netlink_connect( int mode );

/* fdgen_create_veth_pair creates a veth interface pair.  Attempts to
   create an interface with name `fdgen_veth_name` appears in network
   namespaces at file descriptors netns1 and netns2.  (Use
   open(/proc/pid/ns/net) to acquire these FDs).  Returns 0 on success.
   On failure, logs error details to warning log and returns -1. */

extern char const fdgen_veth_name[10];
int
fdgen_create_veth_pair( int netlink,  /* NETLINK_ROUTE */
                        int netns1,
                        int netns2 );

FD_PROTOTYPES_END
