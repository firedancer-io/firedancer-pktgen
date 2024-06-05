#if !defined(__bpf__)
#error "fd_xdp_ports_redirect requires eBPF target"
#endif

#include "fd_ebpf_base.h"
#include <linux/bpf.h>

/* Metadata ***********************************************************/

char __license[] __attribute__(( section("license") )) = "Apache-2.0";

/* eBPF syscalls ******************************************************/

static void *
(* bpf_map_lookup_elem)( void *       map,
                         void const * key)
  = (void *)1U;

static long
(* bpf_redirect_map)( void * map,
                      ulong  key,
                      ulong  flags )
  = (void *)51U;

/* eBPF maps **********************************************************/

extern uint   fd_xdp_xsks __attribute__((section("maps")));
extern uint   fd_xdp_ip;
extern ushort fd_xdp_port_lo;
extern ushort fd_xdp_port_hi;

/* Executable Code ****************************************************/

/* fd_xdp_redirect: Entrypoint of redirect XDP program.
   ctx is the XDP context for an Ethernet/IP packet.
   Returns an XDP action code in XDP_{PASS,REDIRECT,DROP}. */
__attribute__(( section("xdp"), used ))
int fd_xdp_redirect( struct xdp_md *ctx ) {

  uchar const * data      = (uchar const*)(ulong)ctx->data;
  uchar const * data_end  = (uchar const*)(ulong)ctx->data_end;

  if( FD_UNLIKELY( data + 14+20+8 > data_end ) ) return XDP_PASS;

  uchar const * iphdr = data + 14U;

  /* Filter for UDP/IPv4 packets.
     Test for ethtype and ipproto in 1 branch */
  uint test_ethip = ( (uint)data[12] << 16u ) | ( (uint)data[13] << 8u ) | (uint)data[23];
  if( FD_UNLIKELY( test_ethip!=0x080011 ) ) return XDP_PASS;

  /* IPv4 is variable-length, so lookup IHL to find start of UDP */
  uint iplen = ( ( (uint)iphdr[0] ) & 0x0FU ) * 4U;
  uchar const * udp = iphdr + iplen;

  /* Ignore if UDP header is too short */
  if( udp+4U > data_end ) return XDP_PASS;

  /* Extract IP dest addr and UDP dest port */
  ulong ip_dstaddr  = *(uint   *)( iphdr+16UL );
  ulong udp_dstport = *(ushort *)( udp+2UL    );
        udp_dstport = __builtin_bswap16( udp_dstport );

  if( ip_dstaddr != fd_xdp_ip      ) return XDP_PASS;
  if( udp_dstport < fd_xdp_port_lo ) return XDP_PASS;
  if( udp_dstport > fd_xdp_port_hi ) return XDP_PASS;

  /* Look up the interface queue to find the socket to forward to */
  uint socket_key = ctx->rx_queue_index;
  return bpf_redirect_map( &fd_xdp_xsks, socket_key, 0 );
}
