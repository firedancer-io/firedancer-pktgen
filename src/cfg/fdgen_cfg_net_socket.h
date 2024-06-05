#pragma once

/* fdgen_cfg_net_socket.h provides APIs for bulk binding to UDP sockets
   using SO_REUSEPORT. */

#include "fdgen_cfg_net.h"

/* fdgen_ports_socket_t owns an array of sockets. */

struct fdgen_ports_socket {
  ulong sock_max;
  ulong sock_cnt;
};

typedef struct fdgen_ports_socket fdgen_ports_socket_t;

FD_PROTOTYPES_BEGIN

/* fdgen_ports_socket_{align,footprint,new,join,leave,delete} are the
   usual Firedancer dynamic object construction API.

   rx_cnt is the number of receive tiles.  Each receive tile handles a
   subset of traffic for a port.  port_cnt is the number of UDP source
   ports.  There will be rx_cnt*port_cnt sockets in total. */

FD_FN_CONST ulong
fdgen_ports_socket_align( void );

FD_FN_CONST ulong
fdgen_ports_socket_footprint( ulong rx_cnt,
                              ulong port_cnt );

void *
fdgen_ports_socket_new( void * shmem,
                        ulong  rx_cnt,
                        ulong  port_cnt );

fdgen_ports_socket_t *
fdgen_ports_socket_join( void * mem );

void *
fdgen_ports_socket_leave( fdgen_ports_socket_t * ports );

void *
fdgen_ports_socket_delete( void * mem );

/* fdgen_ports_socket_fds returns a pointer to the socket array of a
   ports object.  The array is indexed in [0,sock_max).  Only index in
   [0,sock_cnt) are valid file descriptors. */

FD_FN_CONST static inline int *
fdgen_ports_socket_fds( fdgen_ports_socket_t const * ports ) {
  return fd_type_pun( (void *)( (ulong)ports + sizeof(fdgen_ports_socket_t) ) );
}

/* fdgen_ports_socket_init creates an array of sockets.  Each port in
   the port range will share rx_cnt sockets with SO_REUSEPORT.  Returns
   sockets on success.  On failure, closes all sockets created so far,
   logs warning, and returns NULL. */

fdgen_ports_socket_t *
fdgen_ports_socket_init( fdgen_ports_socket_t * sockets,
                         uint                   ip4,
                         fdgen_port_range_t     port_range,
                         ulong                  rx_cnt );

/* fdgen_ports_socket_fini closes all sockets. */

void
fdgen_ports_socket_fini( fdgen_ports_socket_t * sockets );

/* fdgen_ports_socket_epoll_{join,leaves} adds or removes all sockets to
   the epoll file descriptor with event listener EPOLLIN.   Returns 0 on
   success.  On failure, returns errno-compatible error code. */

int
fdgen_ports_socket_epoll_join( fdgen_ports_socket_t const * sockets,
                               int                          epoll_fd );

int
fdgen_ports_socket_epoll_leave( fdgen_ports_socket_t const * sockets,
                                int                          epoll_fd );

FD_PROTOTYPES_END
