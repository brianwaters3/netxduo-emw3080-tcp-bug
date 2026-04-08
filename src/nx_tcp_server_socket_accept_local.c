/*
 * Local fork of NetXDuo's nx_tcp_server_socket_accept.c with a fix for
 * the wedge described in this repo's README.
 *
 * The bug:
 *
 *   When the client's ACK to the server's SYN-ACK is lost (which is
 *   common with WiFi packet loss), accept() times out and returns
 *   NX_NOT_CONNECTED.  The original failure-path code at the end of
 *   accept() resets state back to NX_TCP_LISTEN_STATE and clears the
 *   timer, but leaves the half-open connection state in place:
 *   bound_next is still set, connect_port is still set, the socket is
 *   still in the bound port table.
 *
 *   When the application loops and calls accept() again, the function
 *   sees state==LISTEN_STATE and re-enters the init code (lines
 *   113-179 below).  The init code unconditionally regenerates the
 *   tx_sequence (= a fresh ISN), and the bound_next branch then
 *   increments tx_sequence and emits a NEW SYN-ACK with the new ISN.
 *   The host now has two SYN-ACKs with different ISNs for the same
 *   client port.  After a few retries the socket's connect_port ends
 *   up zeroed and the IP thread wedges entirely (no more TCP, no more
 *   ICMP), requiring a power cycle.
 *
 * The fix:
 *
 *   In the failure path, fully tear down the half-open connection
 *   before resetting state to LISTEN_STATE.  Specifically: remove the
 *   socket from the bound port table, clear connect_port/connect_ip,
 *   and clear bound_next.  After this, the next accept() call sees
 *   bound_next == NULL and falls through to a clean suspension; the
 *   next SYN that arrives is processed by tcp_packet_process listen
 *   branch as a fresh connection request and the SYN-ACK is sent with
 *   a single, consistent ISN from line 778 of tcp_packet_process.c.
 *
 * The Makefile compiles this file FIRST and links with
 * --allow-multiple-definition so this definition wins over the
 * upstream nx_tcp_server_socket_accept.c.  No ST source files are
 * modified.
 */

#define NX_SOURCE_CODE

#include "nx_api.h"
#include "nx_ip.h"
#include "nx_tcp.h"
#include "tx_thread.h"

UINT  _nx_tcp_server_socket_accept(NX_TCP_SOCKET *socket_ptr, ULONG wait_option)
{

NX_IP *ip_ptr;


    /* Pickup the associated IP structure.  */
    ip_ptr =  socket_ptr -> nx_tcp_socket_ip_ptr;

    /* If trace is enabled, insert this event into the trace buffer.  */
    NX_TRACE_IN_LINE_INSERT(NX_TRACE_TCP_SERVER_SOCKET_ACCEPT, ip_ptr, socket_ptr, wait_option, socket_ptr -> nx_tcp_socket_state, NX_TRACE_TCP_EVENTS, 0, 0);

    /* Check if the socket has already made a connection, return successful outcome to accept(). */
    if (socket_ptr -> nx_tcp_socket_state == NX_TCP_ESTABLISHED)
    {
        return(NX_SUCCESS);
    }

    /* Determine if the socket is still in the listen state or has sent a SYN packet out already
       from a previous accept() call on this socket.  */
    if ((socket_ptr -> nx_tcp_socket_state != NX_TCP_LISTEN_STATE) && (socket_ptr -> nx_tcp_socket_state != NX_TCP_SYN_RECEIVED))
    {

        /* Socket has either been closed or in the process of closing*/
        return(NX_NOT_LISTEN_STATE);
    }


    /* Obtain the IP mutex so we can initiate accept processing for this socket.  */
    tx_mutex_get(&(ip_ptr -> nx_ip_protection), TX_WAIT_FOREVER);

    if (socket_ptr -> nx_tcp_socket_state == NX_TCP_LISTEN_STATE)
    {

        /* Setup the initial sequence number.  */
        if (socket_ptr -> nx_tcp_socket_tx_sequence == 0)
        {
            socket_ptr -> nx_tcp_socket_tx_sequence =  (((ULONG)NX_RAND()) << NX_SHIFT_BY_16) & 0xFFFFFFFF;
            socket_ptr -> nx_tcp_socket_tx_sequence |= (ULONG)NX_RAND();
        }
        else
        {
            socket_ptr -> nx_tcp_socket_tx_sequence =  socket_ptr -> nx_tcp_socket_tx_sequence + ((ULONG)(((ULONG)0x10000))) + ((ULONG)NX_RAND());
        }

        /* Ensure the rx window size logic is reset.  */
        socket_ptr -> nx_tcp_socket_rx_window_current =    socket_ptr -> nx_tcp_socket_rx_window_default;
        socket_ptr -> nx_tcp_socket_rx_window_last_sent =  socket_ptr -> nx_tcp_socket_rx_window_default;

        /* If trace is enabled, insert this event into the trace buffer.  */
        NX_TRACE_IN_LINE_INSERT(NX_TRACE_INTERNAL_TCP_STATE_CHANGE, ip_ptr, socket_ptr, socket_ptr -> nx_tcp_socket_state, NX_TCP_SYN_RECEIVED, NX_TRACE_INTERNAL_EVENTS, 0, 0);

        /* Move the TCP state to Sequence Received, the next state of a passive open.  */
        socket_ptr -> nx_tcp_socket_state =  NX_TCP_SYN_RECEIVED;

        /* Clear the FIN received flag.  */
        socket_ptr -> nx_tcp_socket_fin_received =  NX_FALSE;
        socket_ptr -> nx_tcp_socket_fin_acked =  NX_FALSE;

        /* Determine if the listen command has completed.  This can be detected by checking
           to see if the socket is bound.  If it is bound and still in the listen state, then
           we know that this service is being called after a client connection request was
           received.  */
        if (socket_ptr -> nx_tcp_socket_bound_next)
        {

            /* Send a SYN message back to establish the connection, but increment the ACK first.  */
            socket_ptr -> nx_tcp_socket_rx_sequence++;

            /* Increment the sequence number.  */
            socket_ptr -> nx_tcp_socket_tx_sequence++;

            /* Setup a timeout so the connection attempt can be sent again.  */
            socket_ptr -> nx_tcp_socket_timeout =          socket_ptr -> nx_tcp_socket_timeout_rate;
            socket_ptr -> nx_tcp_socket_timeout_retries =  0;

            /* CLEANUP: Clean up any existing socket data before making a new connection. */
            socket_ptr -> nx_tcp_socket_tx_window_congestion = 0;
            socket_ptr -> nx_tcp_socket_tx_outstanding_bytes = 0;
            socket_ptr -> nx_tcp_socket_packets_sent = 0;
            socket_ptr -> nx_tcp_socket_bytes_sent = 0;
            socket_ptr -> nx_tcp_socket_packets_received = 0;
            socket_ptr -> nx_tcp_socket_bytes_received = 0;
            socket_ptr -> nx_tcp_socket_retransmit_packets = 0;
            socket_ptr -> nx_tcp_socket_checksum_errors = 0;
            socket_ptr -> nx_tcp_socket_transmit_sent_head  =  NX_NULL;
            socket_ptr -> nx_tcp_socket_transmit_sent_tail  =  NX_NULL;
            socket_ptr -> nx_tcp_socket_transmit_sent_count =  0;
            socket_ptr -> nx_tcp_socket_receive_queue_count =  0;
            socket_ptr -> nx_tcp_socket_receive_queue_head  =  NX_NULL;
            socket_ptr -> nx_tcp_socket_receive_queue_tail  =  NX_NULL;

            /* Send the SYN+ACK message.  */
            _nx_tcp_packet_send_syn(socket_ptr, (socket_ptr -> nx_tcp_socket_tx_sequence - 1));
        }
        else
        {
            socket_ptr -> nx_tcp_socket_timeout = 0;
        }
    }

    /* Determine if the wait option is specified.  If so, suspend the calling thread.
       Otherwise, return an in progress status.  */
    if ((wait_option) && (_tx_thread_current_ptr != &(ip_ptr -> nx_ip_thread)))
    {

        /* Suspend the thread on this socket's receive queue.  */
        _nx_tcp_socket_thread_suspend(&(socket_ptr -> nx_tcp_socket_connect_suspended_thread), _nx_tcp_connect_cleanup,
                                      socket_ptr, &(ip_ptr -> nx_ip_protection), wait_option);

        /* Check if the socket connection has failed.  */
        if (_tx_thread_current_ptr -> tx_thread_suspend_status)
        {

            /* If trace is enabled, insert this event into the trace buffer.  */
            NX_TRACE_IN_LINE_INSERT(NX_TRACE_INTERNAL_TCP_STATE_CHANGE, ip_ptr, socket_ptr, socket_ptr -> nx_tcp_socket_state, NX_TCP_LISTEN_STATE, NX_TRACE_INTERNAL_EVENTS, 0, 0);

            /* === LOCAL FIX (vs upstream): fully tear down any half-open
             * connection that didn't complete the 3-way handshake before
             * returning to LISTEN_STATE.  The upstream code only resets
             * state and timeout, leaving bound_next, connect_port and the
             * bound-port-table linkage in place — which causes the next
             * accept() call to enter the bound_next branch above and
             * regenerate the ISN, sending a phantom new SYN-ACK and
             * gradually corrupting socket state. */
            {
                /* Re-acquire the IP mutex (it was released by suspend). */
                tx_mutex_get(&(ip_ptr -> nx_ip_protection), TX_WAIT_FOREVER);

                /* Remove the socket from the bound port table if it's still bound. */
                if (socket_ptr -> nx_tcp_socket_bound_next)
                {
                    UINT port  = socket_ptr -> nx_tcp_socket_port;
                    UINT index = (UINT)((port + (port >> 8)) & NX_TCP_PORT_TABLE_MASK);

                    if (socket_ptr -> nx_tcp_socket_bound_next == socket_ptr)
                    {
                        /* Only socket on this port. */
                        ip_ptr -> nx_ip_tcp_port_table[index] = NX_NULL;
                    }
                    else
                    {
                        /* Relink neighbors. */
                        (socket_ptr -> nx_tcp_socket_bound_next) -> nx_tcp_socket_bound_previous =
                            socket_ptr -> nx_tcp_socket_bound_previous;
                        (socket_ptr -> nx_tcp_socket_bound_previous) -> nx_tcp_socket_bound_next =
                            socket_ptr -> nx_tcp_socket_bound_next;

                        if (ip_ptr -> nx_ip_tcp_port_table[index] == socket_ptr)
                        {
                            ip_ptr -> nx_ip_tcp_port_table[index] =
                                socket_ptr -> nx_tcp_socket_bound_next;
                        }
                    }
                    socket_ptr -> nx_tcp_socket_bound_next     = NX_NULL;
                    socket_ptr -> nx_tcp_socket_bound_previous = NX_NULL;
                }

                /* Clear the connect tuple so the next accept() starts fresh. */
                socket_ptr -> nx_tcp_socket_connect_ip.nxd_ip_version = 0;
#ifdef FEATURE_NX_IPV6
                SET_UNSPECIFIED_ADDRESS(socket_ptr -> nx_tcp_socket_connect_ip.nxd_ip_address.v6);
#else
                socket_ptr -> nx_tcp_socket_connect_ip.nxd_ip_address.v4 = 0;
#endif
                socket_ptr -> nx_tcp_socket_connect_port    = 0;
                socket_ptr -> nx_tcp_socket_timeout         = 0;
                socket_ptr -> nx_tcp_socket_timeout_retries = 0;

                /* Re-arm the listen request so a new connection can come in.
                 * Walk active listen requests and find the one for this port,
                 * then re-attach this socket. */
                {
                    NX_TCP_LISTEN *listen_ptr = ip_ptr -> nx_ip_tcp_active_listen_requests;
                    if (listen_ptr)
                    {
                        do
                        {
                            if ((listen_ptr -> nx_tcp_listen_port == socket_ptr -> nx_tcp_socket_port) &&
                                (listen_ptr -> nx_tcp_listen_socket_ptr == NX_NULL))
                            {
                                listen_ptr -> nx_tcp_listen_socket_ptr = socket_ptr;
                                break;
                            }
                            listen_ptr = listen_ptr -> nx_tcp_listen_next;
                        } while (listen_ptr != ip_ptr -> nx_ip_tcp_active_listen_requests);
                    }
                }

                tx_mutex_put(&(ip_ptr -> nx_ip_protection));
            }
            /* === END LOCAL FIX === */

            /* Yes, socket connection has failed.  Return to the
               listen state so it can be tried again.  */
            socket_ptr -> nx_tcp_socket_state =  NX_TCP_LISTEN_STATE;

            /* Socket is not active. Clear the timeout. */
            socket_ptr -> nx_tcp_socket_timeout =  0;
        }

        /* If not, just return the status.  */
        return(_tx_thread_current_ptr -> tx_thread_suspend_status);
    }
    else
    {

        /* No suspension is request, just release protection and return to the caller.  */

        /* Release the IP protection.  */
        tx_mutex_put(&(ip_ptr -> nx_ip_protection));

        /* Return in-progress completion status.  */
        return(NX_IN_PROGRESS);
    }
}
