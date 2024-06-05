#include "fdgen_cfg_net_socket.h"
#include <firedancer/util/fd_util.h>
#include <firedancer/util/net/fd_ip4.h>

#include <assert.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

FD_FN_CONST ulong
fdgen_ports_socket_align( void ) {
  return alignof(fdgen_ports_socket_t);
}

FD_FN_CONST ulong
fdgen_ports_socket_footprint( ulong rx_cnt,
                              ulong port_cnt ) {

  ulong sock_cnt;
  if( FD_UNLIKELY( __builtin_umull_overflow( rx_cnt, port_cnt, &sock_cnt ) ) ) {
    return 0UL;
  }
  if( FD_UNLIKELY( sock_cnt==0UL ) ) {
    return 0UL;
  }

  ulong footprint;
  if( FD_UNLIKELY( __builtin_umull_overflow( sock_cnt, sizeof(int), &footprint ) ) ) {
    return 0UL;
  }
  if( FD_UNLIKELY( __builtin_uaddl_overflow( footprint, sizeof(fdgen_ports_socket_t), &footprint ) ) ) {
    return 0UL;
  }

  return footprint;
}

void *
fdgen_ports_socket_new( void * shmem,
                        ulong  rx_cnt,
                        ulong  port_cnt ) {

  if( FD_UNLIKELY( !shmem ) ) {
    FD_LOG_WARNING(( "invalid shmem" ));
    return NULL;
  }

  ulong footprint = fdgen_ports_socket_footprint( rx_cnt, port_cnt );
  if( FD_UNLIKELY( !footprint ) ) {
    FD_LOG_WARNING(( "invalid footprint" ));
    return NULL;
  }

  fdgen_ports_socket_t * ports = shmem;
  ports->sock_max = rx_cnt * port_cnt;
  ports->sock_cnt = 0UL;

  int * fds = fdgen_ports_socket_fds( ports );
  for( ulong j = 0UL; j < ports->sock_max; j++ ) {
    fds[j] = -1;
  }

  return ports;
}

fdgen_ports_socket_t *
fdgen_ports_socket_join( void * mem ) {

  if( FD_UNLIKELY( !mem ) ) {
    FD_LOG_WARNING(( "NULL mem" ));
    return NULL;
  }

  return mem;
}

void *
fdgen_ports_socket_leave( fdgen_ports_socket_t * ports ) {
  return ports;
}

void *
fdgen_ports_socket_delete( void * mem ) {

  if( FD_UNLIKELY( !mem ) ) {
    FD_LOG_WARNING(( "NULL mem" ));
    return NULL;
  }

  fdgen_ports_socket_t * ports = mem;
  ports->sock_max = 0UL;
  ports->sock_cnt = 0UL;
  return mem;
}

static int
create_socket( uint listen_ip4,
               uint listen_port ) {

  int sock_fd = socket( AF_INET, SOCK_DGRAM, 0 );
  if( FD_UNLIKELY( sock_fd < 0 ) ) {
    FD_LOG_WARNING(( "socket(AF_INET, SOCK_DGRAM) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
    return -1;
  }

  int reuse = 1;
  if( FD_UNLIKELY( setsockopt( sock_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(int) ) < 0 ) ) {
    FD_LOG_WARNING(( "setsockopt(SO_REUSEPORT) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
    close( sock_fd );
    return -1;
  }

  struct sockaddr_in saddr = {
    .sin_family      = AF_INET,
    .sin_addr.s_addr = listen_ip4,  /* already big endian */
    .sin_port        = fd_ushort_bswap( listen_port )
  };

  if( FD_UNLIKELY( bind( sock_fd, fd_type_pun( &saddr ), sizeof(struct sockaddr_in) ) < 0 ) ) {
    FD_LOG_WARNING(( "bind(" FD_IP4_ADDR_FMT ",%u) failed (%d-%s)",
                     FD_IP4_ADDR_FMT_ARGS( listen_ip4 ), listen_port,
                     errno, fd_io_strerror( errno ) ));
    close( sock_fd );
    return -1;
  }

  return sock_fd;
}

fdgen_ports_socket_t *
fdgen_ports_socket_init( fdgen_ports_socket_t * sockets,
                         uint                   ip4,
                         fdgen_port_range_t     port_range,
                         ulong                  rx_cnt ) {

  ulong port_cnt = fdgen_port_cnt( &port_range );
  ulong sock_cnt;
  if( FD_UNLIKELY( __builtin_umull_overflow( rx_cnt, port_cnt, &sock_cnt )  ) ) {
    FD_LOG_WARNING(( "invalid rx_cnt" ));
    return NULL;
  }
  if( FD_UNLIKELY( sock_cnt==0UL ) ) {
    FD_LOG_WARNING(( "zero port range or zero RX queues" ));
    return NULL;
  }
  if( FD_UNLIKELY( sock_cnt > sockets->sock_max ) ) {
    FD_LOG_WARNING(( "cannot create %lu sockets (%lu ports, %lu RX queues), max is %lu",
                     sock_cnt, port_cnt, rx_cnt, sockets->sock_max ));
    return NULL;
  }

  int * fds = fdgen_ports_socket_fds( sockets );
  for( uint port = port_range.lo; port < port_range.hi; port++ ) {
    for( ulong j=0UL; j<rx_cnt; j++ ) {
      int sock_fd = create_socket( ip4, port );
      if( FD_UNLIKELY( sock_fd < 0 ) ) {
        fdgen_ports_socket_fini( sockets );
        return NULL;
      }
      fds[ sockets->sock_cnt++ ] = sock_fd;
      assert( sockets->sock_cnt <= sockets->sock_max );
    }
  }

  return sockets;
}

void
fdgen_ports_socket_fini( fdgen_ports_socket_t * sockets ) {

  ulong sock_cnt = sockets->sock_cnt;
  int * fds      = fdgen_ports_socket_fds( sockets );

  for( ulong j=0UL; j<sock_cnt; j++ ) {
    if( fds[j] >= 0 ) {
      close( fds[j] );
    }
  }
  sockets->sock_cnt = 0UL;

}

int
fdgen_ports_socket_epoll_join( fdgen_ports_socket_t const * sockets,
                               int                          epoll_fd ) {

  ulong sock_cnt = sockets->sock_cnt;
  int * fds      = fdgen_ports_socket_fds( sockets );

  for( ulong j=0UL; j<sock_cnt; j++ ) {
    struct epoll_event ev = {
      .events  = EPOLLIN,
      .data.fd = fds[j]
    };
    if( FD_UNLIKELY( epoll_ctl( epoll_fd, EPOLL_CTL_ADD, fds[j], &ev )<0 ) ) {
      fdgen_ports_socket_epoll_leave( sockets, epoll_fd );
      FD_LOG_WARNING(( "epoll_ctl(EPOLL_CTL_ADD) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
      return errno;  /* FIXME does fd_log overwrite errno here? */
    }
  }

  return 0;
}

int
fdgen_ports_socket_epoll_leave( fdgen_ports_socket_t const * sockets,
                                int                          epoll_fd ) {

  ulong sock_cnt = sockets->sock_cnt;
  int * fds      = fdgen_ports_socket_fds( sockets );

  for( ulong j=0UL; j<sock_cnt; j++ ) {
    struct epoll_event ev = {
      .events  = EPOLLIN,
      .data.fd = fds[j]
    };

    if( FD_UNLIKELY( epoll_ctl( epoll_fd, EPOLL_CTL_DEL, fds[j], &ev )<0 ) ) {
      FD_LOG_WARNING(( "epoll_ctl(EPOLL_CTL_DEL) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
      return errno;  /* FIXME does fd_log overwrite errno here? */
    }
  }

  return 0;
}
