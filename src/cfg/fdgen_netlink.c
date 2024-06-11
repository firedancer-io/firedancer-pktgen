#define _GNU_SOURCE  /* unshare(2) */
#include "fdgen_netlink.h"

#include <firedancer/util/fd_util.h>
#include <firedancer/util/net/fd_ip4.h>

#include <errno.h>       /* errno(3) */
#include <sched.h>       /* unshare(2) */
#include <fcntl.h>       /* open(2) */
#include <unistd.h>      /* close(2) */

#include <net/if.h>      /* if_nametoindex(3) */

#include <sys/socket.h>  /* socket(2) */

#include <linux/ethtool_netlink.h>  /* ETHTOOL_{...} */
#include <linux/genetlink.h>        /* genlmsghdr */
#include <linux/if_arp.h>           /* ARPHRD_NETROM */
#include <linux/if_link.h>          /* IFLA_{...} */
#include <linux/netlink.h>          /* NLM_{...} */
#include <linux/rtnetlink.h>        /* RTM_{...} */
#include <linux/veth.h>             /* VETH_{...} */

int
fdgen_netlink_connect( int mode ) {

  int netlink = socket( AF_NETLINK, SOCK_RAW|SOCK_CLOEXEC, mode );
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

static int
fdgen_netlink_chk_fail( void ) {
  FD_LOG_WARNING(( "insufficient buffer space for netlink request" ));
  return -1;
}

int
fdgen_netlink_create_veth_pair( int                 netlink,
                                fdgen_veth_params_t params[2] ) {

  /* Assemble netlink message: Create new link (RTM_NEWLINK) */

  do {
    uchar   req_buf[ 1024 ] = {0};
    uchar * end = req_buf + sizeof(req_buf);

#   define BOUNDS_CHECK( ptr ) \
      if( FD_UNLIKELY( (ulong)(ptr) >= (ulong)end ) ) return fdgen_netlink_chk_fail()

    struct nlmsghdr * nlh = fd_type_pun( req_buf );
    BOUNDS_CHECK( nlh+1 );

    nlh->nlmsg_type  = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_EXCL | NLM_F_CREATE;
    nlh->nlmsg_seq   = (uint)fd_tickcount();

    struct ifinfomsg * ifi = NLMSG_DATA( nlh );
    BOUNDS_CHECK( ifi+1 );

    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_type   = ARPHRD_NETROM;

    /* RTM_NEWLINK -> IFLA_NET_NS_FD
       Set local network namespace */

    struct rtattr * rta = fd_type_pun( (uchar *)ifi + NLMSG_ALIGN( sizeof(struct ifinfomsg) ) );
    BOUNDS_CHECK( (ulong)rta + NLA_HDRLEN + sizeof(int) );

    rta->rta_type = IFLA_NET_NS_FD;
    rta->rta_len  = NLA_HDRLEN + sizeof(int);
    FD_STORE( int, RTA_DATA( rta ), params[0].netns );

    /* RTM_NEWLINK -> IFLA_NUM_RX_QUEUES
       Set local rx queue count */

    rta = fd_type_pun( (uchar *)rta + RTA_ALIGN( rta->rta_len ) );
    BOUNDS_CHECK( (ulong)rta + NLA_HDRLEN + sizeof(int) );

    rta->rta_type = IFLA_NUM_RX_QUEUES;
    rta->rta_len  = NLA_HDRLEN + sizeof(int);
    FD_STORE( uint, RTA_DATA( rta ), params[0].rx_queue_cnt );

    /* RTM_NEWLINK -> IFLA_NUM_TX_QUEUES
       Set local tx queue count */

    rta = fd_type_pun( (uchar *)rta + RTA_ALIGN( rta->rta_len ) );
    BOUNDS_CHECK( (ulong)rta + NLA_HDRLEN + sizeof(int) );

    rta->rta_type = IFLA_NUM_TX_QUEUES;
    rta->rta_len  = NLA_HDRLEN + sizeof(int);
    FD_STORE( uint, RTA_DATA( rta ), params[0].tx_queue_cnt );

    /* RTM_NEWLINK -> IFLA_ADDRESS
       Set local MAC address */

    rta = fd_type_pun( (uchar *)rta + RTA_ALIGN( rta->rta_len ) );
    BOUNDS_CHECK( (ulong)rta + NLA_HDRLEN + 6 );

    rta->rta_type = IFLA_ADDRESS;
    rta->rta_len  = NLA_HDRLEN + 6;
    memcpy( RTA_DATA( rta ), params[0].mac_addr, 6 );

    /* RTM_NEWLINK -> IFLA_IFNAME
       Set local interface name */

    rta = fd_type_pun( (uchar *)rta + RTA_ALIGN( rta->rta_len ) );
    BOUNDS_CHECK( (ulong)rta + NLA_HDRLEN + params[0].name_sz );

    rta->rta_type = IFLA_IFNAME;
    rta->rta_len  = NLA_HDRLEN + params[0].name_sz;
    fd_memcpy( RTA_DATA( rta ), params[0].name, params[0].name_sz );

    /* RTM_NEWLINK -> IFLA_LINKINFO
       Interface-specific info */

    rta = fd_type_pun( (uchar *)rta + RTA_ALIGN( rta->rta_len ) );
    BOUNDS_CHECK( (ulong)rta + NLA_HDRLEN );

    rta->rta_type = IFLA_LINKINFO;

    /* RTM_NEWLINK -> IFLA_LINKINFO -> IFLA_INFO_KIND:
       Set interface-specific info to "veth" */

    struct rtattr * rta2 = fd_type_pun( RTA_DATA( rta ) );
    BOUNDS_CHECK( (ulong)rta2 + NLA_HDRLEN + 4 );

    rta2->rta_type = IFLA_INFO_KIND;
    rta2->rta_len  = NLA_HDRLEN + 4;
    fd_memcpy( RTA_DATA( rta2 ), "veth", 4 );

    /* RTM_NEWLINK -> IFLA_LINKINFO -> IFLA_INFO_DATA
       veth info payload */

    rta2 = fd_type_pun( (uchar *)rta2 + RTA_ALIGN( rta2->rta_len ) );
    BOUNDS_CHECK( (ulong)rta2 + NLA_HDRLEN );

    rta2->rta_type = IFLA_INFO_DATA;

    /* RTM_NEWLINK -> IFLA_LINKINFO -> IFLA_INFO_DATA(veth) -> VETH_INFO_PEER
       veth peer payload */

    struct rtattr * rta3 = fd_type_pun( RTA_DATA( rta2 ) );
    BOUNDS_CHECK( (ulong)rta3 + NLA_HDRLEN );

    rta3->rta_type = VETH_INFO_PEER;

    /* ... -> VETH_INFO_PEER -> ifinfomsg */

    struct ifinfomsg * ifi2 = RTA_DATA( rta3 );

    /* ... -> VETH_INFO_PEER -> IFLA_NET_NS_FD
       Set remote network namespace */

    struct rtattr * rta4 = fd_type_pun( ifi2+1 );
    BOUNDS_CHECK( (ulong)rta4 + NLA_HDRLEN + sizeof(int) );

    rta4->rta_type = IFLA_NET_NS_FD;
    rta4->rta_len  = NLA_HDRLEN + sizeof(int);
    FD_STORE( int, RTA_DATA( rta4 ), params[1].netns );

    /* ... -> VETH_INFO_PEER -> IFLA_NUM_RX_QUEUES
       Set remote rx queue count */

    rta4 = fd_type_pun( (uchar *)rta4 + RTA_ALIGN( rta4->rta_len ) );
    BOUNDS_CHECK( (ulong)rta4 + NLA_HDRLEN + sizeof(int) );

    rta4->rta_type = IFLA_NUM_RX_QUEUES;
    rta4->rta_len  = NLA_HDRLEN + sizeof(int);
    FD_STORE( int, RTA_DATA( rta4 ), params[1].rx_queue_cnt );

    /* ... -> VETH_INFO_PEER -> IFLA_NUM_TX_QUEUES
       Set remote tx queue count */

    rta4 = fd_type_pun( (uchar *)rta4 + RTA_ALIGN( rta4->rta_len ) );
    BOUNDS_CHECK( (ulong)rta4 + NLA_HDRLEN + sizeof(int) );

    rta4->rta_type = IFLA_NUM_TX_QUEUES;
    rta4->rta_len  = NLA_HDRLEN + sizeof(int);
    FD_STORE( int, RTA_DATA( rta4 ), params[1].tx_queue_cnt );

    /* ... -> VETH_INFO_PEER -> IFLA_ADDRESS
       Set remote MAC address */

    rta4 = fd_type_pun( (uchar *)rta4 + RTA_ALIGN( rta4->rta_len ) );
    BOUNDS_CHECK( (ulong)rta4 + NLA_HDRLEN + 6 );

    rta4->rta_type = IFLA_ADDRESS;
    rta4->rta_len  = NLA_HDRLEN + 6;
    memcpy( RTA_DATA( rta4 ), params[1].mac_addr, 6 );

    /* ... -> VETH_INFO_PEER -> IFLA_IFNAME
       Set remote interface name */

    rta4 = fd_type_pun( (uchar *)rta4 + RTA_ALIGN( rta4->rta_len ) );
    BOUNDS_CHECK( (ulong)rta4 + NLA_HDRLEN + params[1].name_sz );

    rta4->rta_type = IFLA_IFNAME;
    rta4->rta_len  = NLA_HDRLEN + params[1].name_sz;
    fd_memcpy( RTA_DATA( rta4 ), params[1].name, params[1].name_sz );

    /* Set length of RTM_NEWLINK -> IFLA_LINKINFO -> IFLA_INFO_DATA(veth) -> VETH_INFO_PEER */

    ulong rta3_len = (ulong)rta4 + rta4->rta_len - (ulong)rta3;
    if( FD_UNLIKELY( rta3_len > USHORT_MAX ) ) return fdgen_netlink_chk_fail();

    rta3->rta_len = (ushort)rta3_len;  /* FIXME padding? */

    /* Set length of RTM_NEWLINK -> IFLA_LINKINFO -> IFLA_INFO_DATA(veth) */

    ulong rta2_len = (ulong)rta3 + rta3->rta_len - (ulong)rta2;
    if( FD_UNLIKELY( rta2_len > USHORT_MAX ) ) return fdgen_netlink_chk_fail();

    rta2->rta_len = (ushort)rta2_len;  /* FIXME padding? */

    /* Set length of RTM_NEWLINK -> IFLA_LINKINFO */

    ulong rta_len = (ulong)rta2 + rta2->rta_len - (ulong)rta;
    if( FD_UNLIKELY( rta_len > USHORT_MAX ) ) return fdgen_netlink_chk_fail();

    rta->rta_len = (ushort)rta_len;  /* FIXME padding? */

    /* Set netlink message len */

    ulong nlh_tot_sz = (ulong)rta + rta->rta_len - (ulong)req_buf;
    if( FD_UNLIKELY( nlh_tot_sz > UINT_MAX ) ) return fdgen_netlink_chk_fail();
    nlh->nlmsg_len = (uint)nlh_tot_sz;  /* FIXME padding? */

    /* Send netlink message */

#   undef BOUNDS_CHECK
    FD_LOG_HEXDUMP_DEBUG(( "NETLINK_ROUTE RTM_NEWLINK req", nlh, nlh->nlmsg_len ));

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

    FD_LOG_HEXDUMP_DEBUG(( "NETLINK_ROUTE RTM_NEWLINK resp", res_buf, res_sz ));

    struct nlmsghdr const * nlh = fd_type_pun_const( res_buf );
    FD_TEST( (ulong)(nlh+1) <= (ulong)end );

    if( FD_UNLIKELY( nlh->nlmsg_type != NLMSG_ERROR ) ) {
      FD_LOG_WARNING(( "Unexpected netlink message type (%d)", nlh->nlmsg_type ));
      return -1;
    }

    struct nlmsgerr const * nle = NLMSG_DATA( nlh );
    FD_TEST( (ulong)(nle+1) <= (ulong)end );

    if( FD_UNLIKELY( nle->error != 0 ) ) {
      FD_LOG_WARNING(( "RTM_NEWLINK(veth) failed (%d)", nle->error ));
      return -1;
    }
  } while(0);

  return 0;
}

static int
fdgen_netlink_rtm_newaddr( int  netlink,
                           uint if_idx,
                           uint ip_addr ) {

  /* Assemble netlink message: Create new address (RTM_NEWADDR) */

  do {
    uchar   req_buf[ 1024 ] = {0};
    uchar * end = req_buf + sizeof(req_buf);

#   define BOUNDS_CHECK( ptr ) \
      if( FD_UNLIKELY( (ulong)(ptr) >= (ulong)end ) ) return fdgen_netlink_chk_fail()

    struct nlmsghdr * nlh = fd_type_pun( req_buf );
    BOUNDS_CHECK( nlh+1 );

    nlh->nlmsg_type  = RTM_NEWADDR;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_EXCL | NLM_F_CREATE;
    nlh->nlmsg_seq   = (uint)fd_tickcount();

    struct ifaddrmsg * ifa = NLMSG_DATA( nlh );
    BOUNDS_CHECK( ifa+1 );

    ifa->ifa_family    = AF_INET;
    ifa->ifa_prefixlen = 31;
    ifa->ifa_scope     = RT_SCOPE_LINK;
    ifa->ifa_index     = if_idx;

    /* RTM_NEWADDR -> IFA_LOCAL */

    struct nlattr * nla = fd_type_pun( (uchar *)ifa + NLMSG_ALIGN( sizeof(struct ifaddrmsg) ) );
    BOUNDS_CHECK( (ulong)nla + NLA_HDRLEN + sizeof(uint) );

    nla->nla_type = IFA_LOCAL;
    nla->nla_len  = NLA_HDRLEN + sizeof(uint);
    FD_STORE( uint, fd_type_pun( nla+1 ), ip_addr );

    /* RTM_NEWADDR -> IFA_ADDRESS */

    nla = fd_type_pun( (uchar *)nla + NLA_ALIGN( nla->nla_len ) );
    BOUNDS_CHECK( (ulong)nla + NLA_HDRLEN + sizeof(uint) );

    nla->nla_type = IFA_ADDRESS;
    nla->nla_len  = NLA_HDRLEN + sizeof(uint);
    FD_STORE( uint, fd_type_pun( nla+1 ), ip_addr );

    /* Set netlink message len */

    ulong nlh_tot_sz = (ulong)nla + nla->nla_len - (ulong)req_buf;
    if( FD_UNLIKELY( nlh_tot_sz > UINT_MAX ) ) return fdgen_netlink_chk_fail();
    nlh->nlmsg_len = (uint)nlh_tot_sz;  /* FIXME padding? */

    /* Send netlink message */

#   undef BOUNDS_CHECK
    FD_LOG_HEXDUMP_DEBUG(( "NETLINK_ROUTE RTM_NEWADDR req", nlh, nlh->nlmsg_len ));

    if( FD_UNLIKELY( send( netlink, nlh, nlh->nlmsg_len, 0 )<0 ) ) {
      FD_LOG_WARNING(( "send(RTM_NEWADDR) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
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

    FD_LOG_HEXDUMP_DEBUG(( "NETLINK_ROUTE RTM_NEWADDR resp", res_buf, res_sz ));

    struct nlmsghdr const * nlh = fd_type_pun_const( res_buf );
    FD_TEST( (ulong)(nlh+1) <= (ulong)end );

    if( FD_UNLIKELY( nlh->nlmsg_type != NLMSG_ERROR ) ) {
      FD_LOG_WARNING(( "Unexpected netlink message type (%d)", nlh->nlmsg_type ));
      return -1;
    }

    struct nlmsgerr const * nle = NLMSG_DATA( nlh );
    FD_TEST( (ulong)(nle+1) <= (ulong)end );

    if( FD_UNLIKELY( nle->error != 0 ) ) {
      FD_LOG_WARNING(( "RTM_NEWADDR failed (%d-%s)", nle->error, fd_io_strerror( -nle->error ) ));
      return -1;
    }
  } while(0);

  return 0;
}

static int
fdgen_netlink_link_up( int  netlink,
                       uint if_idx ) {

  /* Assemble netlink message */

  do {
    uchar req_buf[ 1024 ] = {0};

    struct nlmsghdr * nlh = fd_type_pun( req_buf );

    nlh->nlmsg_type  = RTM_NEWLINK;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_len   = NLMSG_LENGTH( sizeof(struct ifinfomsg) );
    nlh->nlmsg_seq   = (uint)fd_tickcount();

    struct ifinfomsg * ifi = NLMSG_DATA( nlh );

    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_type   = ARPHRD_NETROM;
    ifi->ifi_index  = if_idx;
    ifi->ifi_flags  = IFF_UP;
    ifi->ifi_change = IFF_UP;

    FD_LOG_HEXDUMP_DEBUG(( "NETLINK_ROUTE IFF_UP req", nlh, nlh->nlmsg_len ));

    if( FD_UNLIKELY( send( netlink, nlh, nlh->nlmsg_len, 0 )<0 ) ) {
      FD_LOG_WARNING(( "send(RTM_NEWLINK,IFF_UP) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
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

    FD_LOG_HEXDUMP_DEBUG(( "NETLINK_ROUTE IFF_UP resp", res_buf, res_sz ));

    struct nlmsghdr const * nlh = fd_type_pun_const( res_buf );
    FD_TEST( (ulong)(nlh+1) <= (ulong)end );

    if( FD_UNLIKELY( nlh->nlmsg_type != NLMSG_ERROR ) ) {
      FD_LOG_WARNING(( "Unexpected netlink message type (%d)", nlh->nlmsg_type ));
      return -1;
    }

    struct nlmsgerr const * nle = NLMSG_DATA( nlh );
    FD_TEST( (ulong)(nle+1) <= (ulong)end );

    if( FD_UNLIKELY( nle->error != 0 ) ) {
      FD_LOG_WARNING(( "RTM_NEWLINK(veth) failed (%d)", nle->error ));
      return -1;
    }
  } while(0);

  return 0;
}

/* fdgen_nlctrl_get_ethtool queries the ID of the ethtool netlink
   family.  Why are these IDs variable?  sigh ... */

static uint
fdgen_nlctrl_get_ethtool( int netlink ) {

  /* Assemble netlink message */

  do {
    uchar req_buf[ 1024 ] = {0};

    struct nlmsghdr * nlh = fd_type_pun( req_buf );

    nlh->nlmsg_type  = GENL_ID_CTRL;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_seq   = (uint)fd_tickcount();

    struct genlmsghdr * genl = NLMSG_DATA( nlh );

    genl->cmd = CTRL_CMD_GETFAMILY;

    struct nlattr * nla = fd_type_pun( (uchar *)genl + NLMSG_ALIGN( sizeof(struct genlmsghdr) ) );

    nla->nla_type = CTRL_ATTR_FAMILY_NAME;
    nla->nla_len  = NLA_HDRLEN + 8;
    fd_memcpy( RTA_DATA( nla ), "ethtool", 8 );

    nlh->nlmsg_len = (uint)( (ulong)nla + nla->nla_len - (ulong)nlh );

    FD_LOG_HEXDUMP_DEBUG(( "CTRL_CMD_GETFAMILY req", nlh, nlh->nlmsg_len ));

    if( FD_UNLIKELY( send( netlink, nlh, nlh->nlmsg_len, 0 )<0 ) ) {
      FD_LOG_WARNING(( "send(CTRL_CMD_GETFAMILY) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
      return -1;
    }
  } while(0);

  /* Read response */

  do {
    uchar res_buf[ 1024 ] = {0};
    long res_sz = recv( netlink, res_buf, sizeof(res_buf), 0 );
    if( FD_UNLIKELY( res_sz<0L ) ) {
      FD_LOG_WARNING(( "recv(CTRL_CMD_GETFAMILY) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
      return -1;
    }
    uchar const * end = res_buf + res_sz;

    FD_LOG_HEXDUMP_DEBUG(( "CTRL_CMD_GETFAMILY resp", res_buf, res_sz ));

    struct nlmsghdr const * nlh = fd_type_pun_const( res_buf );
    FD_TEST( (ulong)(nlh+1) <= (ulong)end );

    FD_TEST( nlh->nlmsg_type == GENL_ID_CTRL );

    struct genlmsghdr const * genl = NLMSG_DATA( nlh );
    FD_TEST( (ulong)(genl+1) <= (ulong)end );

    FD_TEST( genl->cmd == CTRL_CMD_NEWFAMILY );

    long len = (long)nlh->nlmsg_len - (long)NLMSG_LENGTH( sizeof(struct genlmsghdr) );
    struct rtattr * rta = fd_type_pun( (uchar *)genl + NLMSG_ALIGN( sizeof(struct genlmsghdr) ) );
    for( ; RTA_OK( rta, len ); rta = RTA_NEXT( rta, len ) ) {
      if( rta->rta_type == CTRL_ATTR_FAMILY_ID && rta->rta_len >= sizeof(uint) ) {
        return *(uint *)RTA_DATA( rta );
      }
    }
    FD_LOG_ERR(( "Failed to find netlink family ID of ethtool" ));
  } while(0);

  return 0;
}

static int
fdgen_ethtool_feature_set( int          netlink,
                           uint         nlmsg_type,
                           uint         if_idx,
                           char const * feature,
                           ulong        feature_sz,
                           int          enable ) {

  /* Assemble netlink message */

  do {
    uchar req_buf[ 1024 ] = {0};

    struct nlmsghdr * nlh = fd_type_pun( req_buf );

    nlh->nlmsg_type  = nlmsg_type;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_len   = NLMSG_LENGTH( NLMSG_ALIGN( sizeof(struct genlmsghdr) ) );
    nlh->nlmsg_seq   = (uint)fd_tickcount();

    struct genlmsghdr * genl = NLMSG_DATA( nlh );

    genl->cmd     = ETHTOOL_MSG_FEATURES_SET;
    genl->version = ETHTOOL_GENL_VERSION;

    /* FEATURES_HEADER */

    struct nlattr * nla =  fd_type_pun( (uchar *)genl + NLMSG_ALIGN( sizeof(struct genlmsghdr) ) );

    nla->nla_type = ETHTOOL_A_FEATURES_HEADER | NLA_F_NESTED;

    /* FEATURES_HEADER -> HEADER_DEV_INDEX */

    struct nlattr * nla2 = fd_type_pun( RTA_DATA( nla ) );

    nla2->nla_type = ETHTOOL_A_HEADER_DEV_INDEX;
    nla2->nla_len  = NLA_HDRLEN + sizeof(uint);
    FD_STORE( uint, RTA_DATA( nla2 ), if_idx );

    /* FEATURES_HEADER -> HEADER_FLAGS */

    nla2 = fd_type_pun( (uchar *)nla2 + NLA_ALIGN( nla2->nla_len ) );

    nla2->nla_type = ETHTOOL_A_HEADER_FLAGS;
    nla2->nla_len  = NLA_HDRLEN + sizeof(uint);
    FD_STORE( uint, RTA_DATA( nla2 ), ETHTOOL_FLAG_COMPACT_BITSETS );

    nla->nla_len = (ushort)( (ulong)nla2 + nla2->nla_len - (ulong)nla );

    /* ETHTOOL_A_FEATURES_WANTED */

    nla = fd_type_pun( (uchar *)nla + NLA_ALIGN( nla->nla_len ) );

    nla->nla_type = ETHTOOL_A_FEATURES_WANTED | NLA_F_NESTED;

    /* ETHTOOL_A_FEATURES_WANTED -> ETHTOOL_A_BITSET_BITS */

    nla2 = fd_type_pun( RTA_DATA( nla ) );

    nla2->nla_type = ETHTOOL_A_BITSET_BITS | NLA_F_NESTED;

    /* ETHTOOL_A_FEATURES_WANTED -> ETHTOOL_A_BITSET_BITS
       -> ETHTOOL_A_BITSET_BITS_BIT  */

    struct nlattr * nla3 = fd_type_pun( RTA_DATA( nla2 ) );

    nla3->nla_type = ETHTOOL_A_BITSET_BITS_BIT | NLA_F_NESTED;

    /* ETHTOOL_A_FEATURES_WANTED -> ETHTOOL_A_BITSET_BITS
       -> ETHTOOL_A_BITSET_BITS_BIT -> ETHTOOL_A_BITSET_BIT_NAME */

    struct nlattr * nla4 = fd_type_pun( RTA_DATA( nla3) );

    nla4->nla_type = ETHTOOL_A_BITSET_BIT_NAME;
    nla4->nla_len  = NLA_HDRLEN + feature_sz;
    fd_memcpy( RTA_DATA( nla4 ), feature, feature_sz );

    /* Absence of ETHTOOL_A_BITSET_BIT_VALUE implies false. */

    if( enable ) {

      nla4 = fd_type_pun( (uchar *)nla4 + NLA_ALIGN( nla4->nla_len ) );

      nla4->nla_type = ETHTOOL_A_BITSET_BIT_VALUE;
      nla4->nla_len  = NLA_HDRLEN;

    }

    /* Set lengths */

    nla3->nla_len = (ushort)( (ulong)nla4 + nla4->nla_len - (ulong)nla3 );
    nla2->nla_len = (ushort)( (ulong)nla3 + nla3->nla_len - (ulong)nla2 );
    nla ->nla_len = (ushort)( (ulong)nla2 + nla2->nla_len - (ulong)nla  );

    nlh->nlmsg_len = (uint)( (ulong)nla + nla->nla_len - (ulong)nlh );

    FD_LOG_HEXDUMP_DEBUG(( "ethtool feature set req", nlh, nlh->nlmsg_len ));

    if( FD_UNLIKELY( send( netlink, nlh, nlh->nlmsg_len, 0 )<0 ) ) {
      FD_LOG_WARNING(( "send(ethtool -K %s %s) failed (%d-%s)",
                       feature, enable ? "on" : "off",
                       errno, fd_io_strerror( errno ) ));
      return -1;
    }
  } while(0);

  /* Read response */

  do {
    uchar res_buf[ 1024 ] = {0};
    long res_sz = recv( netlink, res_buf, sizeof(res_buf), 0 );
    if( FD_UNLIKELY( res_sz<0L ) ) {
      FD_LOG_WARNING(( "recv(ethtook -K) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
      return -1;
    }
    uchar const * end = res_buf + res_sz;

    FD_LOG_HEXDUMP_DEBUG(( "ethtool feature set resp", res_buf, res_sz ));
  } while(0);

  return 0;
}

static void
fdgen_netlink_setup_one_veth(
    int          netns,     /* in */
    char const * if_name,   /* in */
    uint *       if_idx_,   /* out */
    uint         ip_addr
) {

  if( FD_UNLIKELY( 0!=setns( netns, CLONE_NEWNET ) )) {
    FD_LOG_ERR(( "setns(CLONE_NEWNET) failed" ));
  }

  uint if_idx = if_nametoindex( if_name );
  *if_idx_ = if_idx;
  if( FD_UNLIKELY( !if_idx ) ) {
    FD_LOG_ERR(( "if_nametoindex(%s) failed", if_name ));
  }

  /* Set link address and bring UP */

  do {

    int nl_route = fdgen_netlink_connect( NETLINK_ROUTE );
    if( FD_UNLIKELY( nl_route<0 ) ) {
      FD_LOG_ERR(( "fdgen_netlink_connect failed" ));
    }

    if( FD_UNLIKELY( 0!=fdgen_netlink_rtm_newaddr( nl_route, if_idx, ip_addr ) ) ) {
      FD_LOG_ERR(( "fdgen_netlink_rtm_newaddr failed" ));
    }

    if( FD_UNLIKELY( 0!=fdgen_netlink_link_up( nl_route, if_idx ) ) ) {
      FD_LOG_ERR(( "fdgen_netlink_link_up failed" ));
    }

    close( nl_route );

  } while(0);

  /* Set ethtool features */

  do {

    int nl_generic = fdgen_netlink_connect( NETLINK_GENERIC );
    if( FD_UNLIKELY( nl_generic<0 ) ) {
      FD_LOG_ERR(( "fdgen_netlink_connect failed" ));
    }

    uint family = fdgen_nlctrl_get_ethtool( nl_generic );

    static char const tx_udp_seg[] = "tx-udp-segmentation";
    static char const gso[]        = "tx-generic-segmentation";
    static char const tx_gre[]     = "tx-gre-segmentation";

    fdgen_ethtool_feature_set( nl_generic, family, if_idx, tx_udp_seg, sizeof(tx_udp_seg), 0 );
    fdgen_ethtool_feature_set( nl_generic, family, if_idx, gso,        sizeof(gso),        0 );
    fdgen_ethtool_feature_set( nl_generic, family, if_idx, tx_gre,     sizeof(tx_gre),     0 );

    close( nl_generic );

  } while(0);
}

void
fdgen_netlink_create_veth_env( fdgen_veth_env_t * env ) {

  /* Create network namespaces */

  int old_ns = open( "/proc/self/ns/net", O_RDONLY );
  if( FD_UNLIKELY( old_ns<0 )) {
    FD_LOG_ERR(( "open(/proc/self/ns/net) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
  }

  int netns[2];

  if( FD_UNLIKELY( 0!=unshare( CLONE_NEWNET ) )) {
    FD_LOG_ERR(( "unshare(CLONE_NEWNET) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
  }

  netns[0] = open( "/proc/self/ns/net", O_RDONLY );
  if( FD_UNLIKELY( netns[0]<0 )) {
    FD_LOG_ERR(( "open(/proc/self/ns/net) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
    /* irrecoverable */
  }

  if( FD_UNLIKELY( 0!=unshare( CLONE_NEWNET ) )) {
    FD_LOG_ERR(( "unshare(CLONE_NEWNET) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
  }

  netns[1] = open( "/proc/self/ns/net", O_RDONLY );
  if( FD_UNLIKELY( netns[1]<0 )) {
    FD_LOG_ERR(( "open(/proc/self/ns/net) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
    /* irrecoverable */
  }

  if( FD_UNLIKELY( 0!=setns( old_ns, CLONE_NEWNET ) )) {
    FD_LOG_ERR(( "setns(CLONE_NEWNET) failed (%d-%s)", errno, fd_io_strerror( errno ) ));
    /* irrecoverable */
  }

  /* Create veth pair */

  env->params[0] = (fdgen_veth_params_t) {
    .name         = "veth",
    .name_sz      = 5,
    .netns        = netns[0],
    .mac_addr     = { 0x0a, 0x00, 0x00, 0x00, 0x00, 0x10 },
    .rx_queue_cnt = env->rx_queue_cnt[0],
    .tx_queue_cnt = env->tx_queue_cnt[0]
  };
  env->ip_addr[0] = FD_IP4_ADDR( 10, 0, 0, 8 );

  env->params[1] = (fdgen_veth_params_t) {
    .name         = "veth",
    .name_sz      = 5,
    .netns        = netns[1],
    .mac_addr     = { 0x0a, 0x00, 0x00, 0x00, 0x00, 0x11 },
    .rx_queue_cnt = env->rx_queue_cnt[1],
    .tx_queue_cnt = env->tx_queue_cnt[1]
  };
  env->ip_addr[1] = FD_IP4_ADDR( 10, 0, 0, 9 );

  int nl_route = fdgen_netlink_connect( NETLINK_ROUTE );
  if( FD_UNLIKELY( nl_route<0 ) ) {
    FD_LOG_ERR(( "fdgen_netlink_connect failed" ));
  }
  if( FD_UNLIKELY( 0!=fdgen_netlink_create_veth_pair( nl_route, env->params ) ) ) {
    FD_LOG_ERR(( "fdgen_netlink_create_veth_pair failed" ));
  }
  close( nl_route );

  /* Further configuration */

  fdgen_netlink_setup_one_veth( netns[0], env->params[0].name, &env->if_idx[0], env->ip_addr[0] );
  fdgen_netlink_setup_one_veth( netns[1], env->params[1].name, &env->if_idx[1], env->ip_addr[1] );

  /* Restore original netns */

  if( FD_UNLIKELY( 0!=setns( old_ns, CLONE_NEWNET ) )) {
    FD_LOG_ERR(( "setns(CLONE_NEWNET) failed" ));
  }

  close( old_ns );
}
