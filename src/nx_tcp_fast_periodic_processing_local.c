/*
 * Local fork of NetXDuo's nx_tcp_fast_periodic_processing.c with the
 * issue #306 workaround applied.
 *
 *   https://github.com/eclipse-threadx/netxduo/issues/306
 *
 * The upstream function (in CubeU5 1.8.0's bundled NetXDuo) only calls
 * _nx_tcp_socket_connection_reset() when retries are exhausted AND a
 * specific zero-window-probe state is satisfied:
 *
 *     else if (((... timeout_retries >= ... max_retries) &&
 *               (... zero_window_probe_has_data == NX_FALSE)) ||
 *              ((... zero_window_probe_failure >= ... max_retries) &&
 *               (... zero_window_probe_has_data == NX_TRUE)))
 *     {
 *         _nx_tcp_socket_connection_reset(socket_ptr);
 *     }
 *
 * Per issue #306, that condition is "never fulfilled" in normal flow,
 * so the connection is never reset, the transmit queue is never
 * flushed, and packets accumulate forever on
 * nx_tcp_socket_transmit_sent_head.  This local fork restores the
 * legacy pre-NetXDuo behavior of just checking timeout_retries
 * against max_retries.
 *
 * The Makefile lists this file FIRST in OBJS and links with
 * --allow-multiple-definition, so the linker picks this definition
 * over the upstream one.  No ST source files are modified.
 */

#define NX_SOURCE_CODE

#include "nx_api.h"
#include "nx_ip.h"
#ifdef FEATURE_NX_IPV6
#include "nx_ipv6.h"
#endif
#include "nx_packet.h"
#include "nx_tcp.h"

VOID _nx_tcp_fast_periodic_processing(NX_IP *ip_ptr)
{
NX_TCP_SOCKET *socket_ptr;
ULONG          sockets;
ULONG          timer_rate;

    timer_rate = _nx_tcp_fast_timer_rate;
    sockets    = ip_ptr -> nx_ip_tcp_created_sockets_count;
    socket_ptr = ip_ptr -> nx_ip_tcp_created_sockets_ptr;

    while (sockets--)
    {
        if ((socket_ptr -> nx_tcp_socket_state >= NX_TCP_ESTABLISHED) &&
            ((socket_ptr -> nx_tcp_socket_rx_sequence != socket_ptr -> nx_tcp_socket_rx_sequence_acked) ||
             (socket_ptr -> nx_tcp_socket_rx_window_last_sent < socket_ptr -> nx_tcp_socket_rx_window_current)))
        {
            if (socket_ptr -> nx_tcp_socket_delayed_ack_timeout <= timer_rate)
            {
                _nx_tcp_packet_send_ack(socket_ptr, socket_ptr -> nx_tcp_socket_tx_sequence);
            }
            else
            {
                socket_ptr -> nx_tcp_socket_delayed_ack_timeout -= timer_rate;
            }
        }

        if (socket_ptr -> nx_tcp_socket_timeout)
        {
            if (socket_ptr -> nx_tcp_socket_timeout > timer_rate)
            {
                socket_ptr -> nx_tcp_socket_timeout -= timer_rate;
            }
            /* WORKAROUND for eclipse-threadx/netxduo#306:
             * the upstream condition gates connection_reset on
             * zero_window_probe_has_data, which is never satisfied in
             * the normal connect/disconnect flow.  Drop the gating and
             * just reset on retry exhaustion. */
            else if (socket_ptr -> nx_tcp_socket_timeout_retries >=
                     socket_ptr -> nx_tcp_socket_timeout_max_retries)
            {
                _nx_tcp_socket_connection_reset(socket_ptr);
            }
            else if ((socket_ptr -> nx_tcp_socket_state == NX_TCP_SYN_SENT) ||
                     (socket_ptr -> nx_tcp_socket_state == NX_TCP_SYN_RECEIVED))
            {
                socket_ptr -> nx_tcp_socket_timeout_retries++;
                socket_ptr -> nx_tcp_socket_timeout = socket_ptr -> nx_tcp_socket_timeout_rate <<
                    (socket_ptr -> nx_tcp_socket_timeout_retries * socket_ptr -> nx_tcp_socket_timeout_shift);
                _nx_tcp_packet_send_syn(socket_ptr, (socket_ptr -> nx_tcp_socket_tx_sequence - 1));
            }
            else if (socket_ptr -> nx_tcp_socket_transmit_sent_head ||
                     ((socket_ptr -> nx_tcp_socket_tx_window_advertised == 0) &&
                      (socket_ptr -> nx_tcp_socket_state <= NX_TCP_CLOSE_WAIT)))
            {
                socket_ptr -> nx_tcp_socket_tx_sequence_recover = socket_ptr -> nx_tcp_socket_tx_sequence - 1;
                _nx_tcp_socket_retransmit(ip_ptr, socket_ptr, NX_FALSE);
                socket_ptr -> nx_tcp_socket_fast_recovery = NX_FALSE;
                socket_ptr -> nx_tcp_socket_tx_window_congestion = socket_ptr -> nx_tcp_socket_tx_slow_start_threshold;
            }
            else if ((socket_ptr -> nx_tcp_socket_state == NX_TCP_FIN_WAIT_1) ||
                     (socket_ptr -> nx_tcp_socket_state == NX_TCP_CLOSING)    ||
                     (socket_ptr -> nx_tcp_socket_state == NX_TCP_LAST_ACK))
            {
                socket_ptr -> nx_tcp_socket_timeout_retries++;
                socket_ptr -> nx_tcp_socket_timeout = socket_ptr -> nx_tcp_socket_timeout_rate <<
                    (socket_ptr -> nx_tcp_socket_timeout_retries * socket_ptr -> nx_tcp_socket_timeout_shift);
                _nx_tcp_packet_send_fin(socket_ptr, (socket_ptr -> nx_tcp_socket_tx_sequence - 1));
            }
            else if (socket_ptr -> nx_tcp_socket_state == NX_TCP_TIMED_WAIT)
            {
                _nx_tcp_socket_block_cleanup(socket_ptr);
            }
        }

        socket_ptr = socket_ptr -> nx_tcp_socket_created_next;
    }
}
