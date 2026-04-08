/*
 * Linker --wrap instrumentation for _nx_tcp_packet_send_syn().
 *
 * The Makefile passes -Wl,--wrap=_nx_tcp_packet_send_syn to the linker.
 * Every call site in NetXDuo to _nx_tcp_packet_send_syn() is rewritten
 * to call __wrap__nx_tcp_packet_send_syn() instead, and the original
 * function is renamed to __real__nx_tcp_packet_send_syn().  This lets
 * us print every SYN-ACK transmission without modifying any ST source.
 *
 * Goal: confirm whether a single failing TCP cycle generates one or
 * two SYN-ACKs from the device, and if two, whether they share a
 * tx_sequence (=> retransmit, RFC-correct) or differ (=> phantom
 * second TCB, the suspected NetXDuo bug).
 */

#include <stdio.h>
#define NX_SOURCE_CODE
#include "tx_api.h"
#include "nx_api.h"
#include "nx_tcp.h"

extern void __real__nx_tcp_packet_send_syn(NX_TCP_SOCKET *socket_ptr,
                                            ULONG tx_sequence);
extern void __real__nx_tcp_packet_process(NX_IP *ip_ptr, NX_PACKET *packet_ptr);
extern UINT __real__nx_tcp_server_socket_accept(NX_TCP_SOCKET *socket_ptr,
                                                 ULONG wait_option);

/* Network-order byte-level flag bits in the TCP header byte at offset 13.
 * NetXDuo's NX_TCP_SYN_BIT etc. are defined as masks against the 32-bit
 * host-order word `nx_tcp_header_word_3` (e.g. NX_TCP_SYN_BIT = 0x00020000),
 * NOT against a single byte — applying them to p[13] always yields 0,
 * which lets GCC eliminate the printf as dead code. */
#define TCP_FIN_BYTE 0x01
#define TCP_SYN_BYTE 0x02
#define TCP_RST_BYTE 0x04
#define TCP_ACK_BYTE 0x10

/* Per-cycle prints are GATED by VERBOSE_LOGS so we can disable them at
 * compile time and check whether the wedge is amplified by the printf
 * load on USART1.  Set VERBOSE_LOGS to 1 to re-enable. */
#ifndef VERBOSE_LOGS
#define VERBOSE_LOGS 0
#endif

/* Marked noinline so GCC -O2 can't tail-call optimize the body away. */
__attribute__((noinline)) static void tcp_in_log(NX_PACKET *packet_ptr)
{
#if VERBOSE_LOGS
    /* `volatile` on the byte pointer prevents GCC from "proving" the
     * loads are dead and eliminating the printf. */
    volatile UCHAR *p = (volatile UCHAR *)packet_ptr->nx_packet_prepend_ptr;
    UINT src_port = ((UINT)p[0] << 8) | p[1];
    UINT dst_port = ((UINT)p[2] << 8) | p[3];
    UINT flags    = p[13];

    if ((flags & TCP_SYN_BYTE) != 0) {
        printf("[tcp_in] pkt=%p sp=%u dp=%u flags=%s%s%s%s\n",
               (void *)packet_ptr, src_port, dst_port,
               (flags & TCP_SYN_BYTE) ? "S" : "",
               (flags & TCP_ACK_BYTE) ? "A" : "",
               (flags & TCP_RST_BYTE) ? "R" : "",
               (flags & TCP_FIN_BYTE) ? "F" : "");
    }
#else
    (void)packet_ptr;
#endif
}

void __wrap__nx_tcp_packet_process(NX_IP *ip_ptr, NX_PACKET *packet_ptr)
{
    if (packet_ptr != NX_NULL && packet_ptr->nx_packet_prepend_ptr != NX_NULL) {
        tcp_in_log(packet_ptr);
    }
    __real__nx_tcp_packet_process(ip_ptr, packet_ptr);
}

/* Forward declarations of NetXDuo APIs we use in the cleanup path. */
UINT _nxe_tcp_socket_disconnect(NX_TCP_SOCKET *, ULONG);
UINT _nxe_tcp_server_socket_unaccept(NX_TCP_SOCKET *);
UINT _nxe_tcp_server_socket_relisten(NX_IP *, UINT, NX_TCP_SOCKET *);
#ifndef NX_WAIT_FOREVER
#define NX_WAIT_FOREVER 0xFFFFFFFFUL
#endif

UINT __wrap__nx_tcp_server_socket_accept(NX_TCP_SOCKET *socket_ptr,
                                          ULONG wait_option)
{
#if VERBOSE_LOGS
    UINT  state    = socket_ptr ? socket_ptr->nx_tcp_socket_state : 0;
    void *bnext    = socket_ptr ? (void *)socket_ptr->nx_tcp_socket_bound_next : NULL;
    UINT  cport    = socket_ptr ? socket_ptr->nx_tcp_socket_connect_port : 0;
    ULONG cv4      = (socket_ptr && socket_ptr->nx_tcp_socket_connect_ip.nxd_ip_version == 4)
                     ? socket_ptr->nx_tcp_socket_connect_ip.nxd_ip_address.v4 : 0;
    printf("[accept_in] sock=%p state=%u bound_next=%p connect=%lu.%lu.%lu.%lu:%u\n",
           (void *)socket_ptr, state, bnext,
           (cv4 >> 24) & 0xff, (cv4 >> 16) & 0xff,
           (cv4 >>  8) & 0xff,  cv4        & 0xff,
           cport);
#endif

    UINT rc = __real__nx_tcp_server_socket_accept(socket_ptr, wait_option);

#if VERBOSE_LOGS
    UINT state2 = socket_ptr ? socket_ptr->nx_tcp_socket_state : 0;
    UINT cport2 = socket_ptr ? socket_ptr->nx_tcp_socket_connect_port : 0;
    printf("[accept_out] sock=%p rc=%u state=%u connect_port=%u\n",
           (void *)socket_ptr, rc, state2, cport2);
#endif

    return rc;
}

void __wrap__nx_tcp_packet_send_syn(NX_TCP_SOCKET *socket_ptr,
                                    ULONG tx_sequence)
{
    /* Connect tuple at the moment of the call.  In SYN_RECEIVED state
     * connect_ip and connect_port are the *peer* of the half-open
     * connection that this SYN-ACK is going to. */
    ULONG peer_v4 = 0;
    UINT peer_port = 0;
    UINT state = 0;
    if (socket_ptr) {
        peer_v4   = socket_ptr->nx_tcp_socket_connect_ip.nxd_ip_address.v4;
        peer_port = socket_ptr->nx_tcp_socket_connect_port;
        state     = socket_ptr->nx_tcp_socket_state;
    }
#if VERBOSE_LOGS
    printf("[syn_wrap] send_syn sock=%p state=%u peer=%lu.%lu.%lu.%lu:%u "
           "tx_seq=%lu\n",
           (void *)socket_ptr, state,
           (peer_v4 >> 24) & 0xff, (peer_v4 >> 16) & 0xff,
           (peer_v4 >>  8) & 0xff,  peer_v4        & 0xff,
           peer_port,
           (unsigned long)tx_sequence);
#else
    (void)peer_v4; (void)peer_port; (void)state; (void)tx_sequence;
#endif

    __real__nx_tcp_packet_send_syn(socket_ptr, tx_sequence);
}
