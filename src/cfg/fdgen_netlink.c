#define _GNU_SOURCE
#include "fdgen_netlink.h"
#include <firedancer/util/fd_util.h>

#include <errno.h>       /* errno(3) */
#include <sched.h>       /* unshare(2) */
#include <fcntl.h>       /* open(2) */

#include <sys/socket.h>  /* socket(2) */

#include <linux/if_arp.h>     /* ARPHRD_NETROM */
#include <linux/if_link.h>    /* IFLA_{...} */
#include <linux/netlink.h>    /* NLM_{...} */
#include <linux/rtnetlink.h>  /* RTM_{...} */

int
fdgen_create_netns( void ) {

  int old_ns = open( "/proc/self/ns/net", O_RDONLY );
  if( FD_UNLIKELY( old_ns<0 )) {
    FD_LOG_WARNING(( "open(/proc/self/ns/net) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
    return -1;
  }

  if( FD_UNLIKELY( 0!=unshare( CLONE_NEWNET ) )) {
    FD_LOG_WARNING(( "unshare(CLONE_NEWNET) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
    return -1;
  }

  int new_ns = open( "/proc/self/ns/net", O_RDONLY );
  if( FD_UNLIKELY( new_ns<0 )) {
    FD_LOG_ERR(( "open(/proc/self/ns/net) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
    /* irrecoverable */
  }

  if( FD_UNLIKELY( 0!=setns( old_ns, CLONE_NEWNET ) )) {
    FD_LOG_ERR(( "setns(CLONE_NEWNET) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
    /* irrecoverable */
  }

  return new_ns;
}

int
fdgen_netlink_connect( int mode ) {

  int netlink = socket( AF_NETLINK, SOCK_RAW, mode );
  if( FD_UNLIKELY( netlink<0 )) {
    FD_LOG_WARNING(( "socket(AF_NETLINK,SOCK_RAW,NETLINK_ROUTE) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
    return -1;
  }

  int ext_ack = 1;
  if( FD_UNLIKELY( 0!=setsockopt( netlink, SOL_NETLINK, NETLINK_EXT_ACK, &ext_ack, sizeof(int) ) ) ) {
    FD_LOG_WARNING(( "setsockopt(SOL_NETLINK,NETLINK_EXT_ACK) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
    return -1;
  }

  int strict_check = 1;
  if( FD_UNLIKELY( 0!=setsockopt( netlink, SOL_NETLINK, NETLINK_GET_STRICT_CHK, &strict_check, sizeof(int) ) ) ) {
    FD_LOG_WARNING(( "setsockopt(SOL_NETLINK,NETLINK_GET_STRICT_CHK) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
    return -1;
  }

  struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
  if( FD_UNLIKELY( 0!=bind( netlink, fd_type_pun_const( &addr ), sizeof(addr) ) ) ) {
    FD_LOG_WARNING(( "bind({.nl_family=AF_NETLINK}) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
    return -1;
  }

  return netlink;
}

char const fdgen_veth_name[10] = "test_veth";

int
fdgen_create_veth_pair( int netlink,  /* NETLINK_ROUTE */
                        int netns1,
                        int netns2 ) {

  /* Assemble netlink message: Create new link (RTM_NEWLINK) */

  do {
    uchar   req_buf[ 1024 ] = {0};
    uchar * end = req_buf + sizeof(req_buf);

    struct nlmsghdr * nlh = fd_type_pun( req_buf );
    FD_TEST( (ulong)(nlh+1) < (ulong)end );

    nlh->nlmsg_type  = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_EXCL | NLM_F_CREATE;

    struct ifinfomsg * ifi = NLMSG_DATA( nlh );
    FD_TEST( (ulong)(ifi+1) < (ulong)end );

    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_type   = ARPHRD_NETROM;

    /* RTM_NEWLINK -> IFLA_NET_NS_FD
      Set local network namespace */

    struct rtattr * rta = fd_type_pun( (uchar *)ifi + NLMSG_ALIGN( sizeof(struct ifinfomsg) ) );
    FD_TEST( (ulong)rta + NLA_HDRLEN + sizeof(int) < (ulong)end );

    rta->rta_type = IFLA_NET_NS_FD;
    rta->rta_len  = NLA_HDRLEN + sizeof(int);
    FD_STORE( int, RTA_DATA( rta ), netns1 );

    /* RTM_NEWLINK -> IFLA_IFNAME
      Set local interface name */

    rta = fd_type_pun( (uchar *)rta + RTA_ALIGN( rta->rta_len ) );
    FD_TEST( (ulong)rta + NLA_HDRLEN + sizeof(fdgen_veth_name) < (ulong)end );

    rta->rta_type = IFLA_IFNAME;
    rta->rta_len  = NLA_HDRLEN + sizeof(fdgen_veth_name);
    fd_memcpy( RTA_DATA( rta ), fdgen_veth_name, sizeof(fdgen_veth_name) );

    /* RTM_NEWLINK -> IFLA_LINKINFO
      Interface-specific info */

    rta = fd_type_pun( (uchar *)rta + RTA_ALIGN( rta->rta_len ) );
    FD_TEST( (ulong)rta + NLA_HDRLEN < (ulong)end );

    rta->rta_type = IFLA_LINKINFO;

    /* RTM_NEWLINK -> IFLA_LINKINFO -> IFLA_INFO_KIND:
      Set interface-specific info to "veth" */

    struct rtattr * rta2 = fd_type_pun( RTA_DATA( rta ) );
    FD_TEST( (ulong)rta2 + NLA_HDRLEN + 4 < (ulong)end );

    rta2->rta_type = IFLA_INFO_KIND;
    rta2->rta_len  = NLA_HDRLEN + 4;
    fd_memcpy( RTA_DATA( rta2 ), "veth", 4 );

    /* RTM_NEWLINK -> IFLA_LINKINFO -> IFLA_INFO_DATA
      veth info payload */

    rta2 = fd_type_pun( (uchar *)rta2 + RTA_ALIGN( rta2->rta_len ) );
    FD_TEST( (ulong)rta2 + NLA_HDRLEN < (ulong)end );

    rta2->rta_type = IFLA_INFO_DATA;

    /* RTM_NEWLINK -> IFLA_LINKINFO -> IFLA_INFO_DATA(veth) -> IFLA_ADDRESS */

    struct rtattr * rta3 = fd_type_pun( RTA_DATA( rta2 ) );
    FD_TEST( (ulong)rta3 + 40 < (ulong)end );

    rta3->rta_type = IFLA_ADDRESS;
    rta3->rta_len  = 40;
    fd_memset( RTA_DATA( rta3 ), 0, 40 );

    /* RTM_NEWLINK -> IFLA_LINKINFO -> IFLA_INFO_DATA(veth) -> IFLA_NET_NS_FD
      Set remote network namespace */

    rta3 = fd_type_pun( (uchar *)rta3 + RTA_ALIGN( rta3->rta_len ) );
    FD_TEST( (ulong)rta3 + NLA_HDRLEN + sizeof(int) < (ulong)end );

    rta3->rta_type = IFLA_NET_NS_FD;
    rta3->rta_len  = NLA_HDRLEN + sizeof(int);
    FD_STORE( int, RTA_DATA( rta3 ), netns2 );

    /* RTM_NEWLINK -> IFLA_LINKINFO -> IFLA_INFO_DATA(veth) -> IFLA_IFNAME
      Set remote interface name */

    rta3 = fd_type_pun( (uchar *)rta3 + RTA_ALIGN( rta3->rta_len ) );
    FD_TEST( (ulong)rta3 + NLA_HDRLEN + sizeof(fdgen_veth_name) < (ulong)end );

    rta3->rta_type = IFLA_IFNAME;
    rta3->rta_len  = NLA_HDRLEN + sizeof(fdgen_veth_name);
    fd_memcpy( RTA_DATA( rta3 ), fdgen_veth_name, sizeof(fdgen_veth_name) );

    /* Set length of RTM_NEWLINK -> IFLA_LINKINFO -> IFLA_INFO_DATA(veth) */

    ulong rta3_tot_sz = (ulong)rta3 + rta3->rta_len - (ulong)RTA_DATA( rta2 );
    FD_TEST( rta3_tot_sz <= USHORT_MAX );

    rta2->rta_len = (ushort)rta3_tot_sz;  /* FIXME padding? */

    /* Set length of RTM_NEWLINK -> IFLA_LINKINFO */

    ulong rta2_tot_sz = (ulong)rta2 + rta2->rta_len - (ulong)RTA_DATA( rta );
    FD_TEST( rta2_tot_sz <= USHORT_MAX );

    rta->rta_len = (ushort)rta2_tot_sz;  /* FIXME padding? */

    /* Set netlink message len */

    ulong nlh_tot_sz = (ulong)rta + rta->rta_len - (ulong)req_buf;
    FD_TEST( nlh_tot_sz <= UINT_MAX );
    nlh->nlmsg_len = (uint)nlh_tot_sz;  /* FIXME padding? */

    /* Send netlink message */

    FD_LOG_HEXDUMP_DEBUG(( "NETLINK_ROUTE request", nlh, nlh->nlmsg_len ));

    if( FD_UNLIKELY( send( netlink, nlh, nlh->nlmsg_len, 0 )<0 ) ) {
      FD_LOG_WARNING(( "send(RTM_NEWLINK) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
      return -1;
    }
  } while(0);

  /* Read response */

  do {
    uchar res_buf[ 1024 ] = {0};
    long res_sz = recv( netlink, res_buf, sizeof(res_buf), 0 );
    if( FD_UNLIKELY( res_sz<0L ) ) {
      FD_LOG_WARNING(( "recv(AF_NETLINK) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
      return -1;
    }
    uchar const * end = res_buf + res_sz;

    FD_LOG_HEXDUMP_DEBUG(( "NETLINK_ROUTE response", res_buf, res_sz ));

    struct nlmsghdr const * nlh = fd_type_pun_const( res_buf );
    FD_TEST( (ulong)(nlh+1) <= (ulong)end );

    if( FD_UNLIKELY( nlh->nlmsg_type != NLMSG_ERROR ) ) {
      FD_LOG_WARNING(( "Unexpected netlink message type (%d)", nlh->nlmsg_type ));
      return -1;
    }

    struct nlmsgerr const * nle = NLMSG_DATA( nlh );
    FD_TEST( (ulong)(nle+1) <= (ulong)end );

    if( FD_UNLIKELY( nle->error != 0 ) ) {
      FD_LOG_WARNING(( "Failed to create veth pair (%d)", nle->error ));
      return -1;
    }
  } while(0);

  FD_LOG_DEBUG(( "success" ));
  return 0;
}
