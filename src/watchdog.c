/*
 * Watchdog/diagnostic thread for the reproducer.
 *
 * Started lazily from inc/app_netxduo.h's PRINT_DATA macro on the
 * first received-byte cycle.  Wakes up every WD_INTERVAL_TICKS ticks
 * and, if PRINT_DATA's heartbeat counter has not advanced since the
 * last wake, dumps every ThreadX thread's name + state directly to
 * USART1 (the ST-Link VCP, bypassing newlib stdio).
 *
 * The intent is to make it visible when the system wedges and to
 * pinpoint *which* thread is stuck and *what* it's blocked on
 * (TX_MUTEX_SUSP / TX_SEMAPHORE_SUSP / TX_SLEEP / TX_TCP_IP / etc.)
 * so the upstream bug report can include a real wedge point instead
 * of just "the TCP server stopped accepting".
 *
 * No ST source files are modified.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "stm32u5xx.h"

/* Need NX_SOURCE_CODE so the NetXDuo internal structs (NX_IP,
 * NX_PACKET_POOL, NX_TCP_SOCKET) are exposed as concrete types
 * rather than opaque forward declarations. */
#define NX_SOURCE_CODE
#include "tx_api.h"
#include "tx_thread.h"
#include "nx_api.h"
#include "nx_ip.h"
#include "nx_tcp.h"

/* NetXDuo internal: head of the global IP instance list. */
extern NX_IP *_nx_ip_created_ptr;
extern ULONG _nx_ip_created_count;

/* Heartbeat counter touched from PRINT_DATA in inc/app_netxduo.h.
 * watchdog_register_heartbeat() reads it. */
extern unsigned long _print_data_count;
unsigned long _print_data_count = 0;

#define WD_STACK_SIZE     1024
#define WD_INTERVAL_TICKS 1000  /* 1 second at TX_TIMER_TICKS_PER_SECOND=1000 */
#define WD_PRIORITY       30    /* lower than the IP thread */

static TX_THREAD wd_thread;
static UCHAR wd_stack[WD_STACK_SIZE];
static int wd_started = 0;

static void wd_putc(char c)
{
    while (!(USART1->ISR & USART_ISR_TXE_TXFNF)) { }
    USART1->TDR = (uint8_t)c;
}

static void wd_puts(const char *s)
{
    while (*s) wd_putc(*s++);
}

static void wd_putu(unsigned long v)
{
    char buf[12];
    int i = 0;
    if (v == 0) { wd_putc('0'); return; }
    while (v && i < (int)sizeof(buf)) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) wd_putc(buf[i]);
}

static const char *thread_state_name(UINT state)
{
    switch (state) {
        case TX_READY:           return "READY";
        case TX_COMPLETED:       return "COMPLETED";
        case TX_TERMINATED:      return "TERMINATED";
        case TX_SUSPENDED:       return "SUSPENDED";
        case TX_SLEEP:           return "SLEEP";
        case TX_QUEUE_SUSP:      return "QUEUE_SUSP";
        case TX_SEMAPHORE_SUSP:  return "SEMA_SUSP";
        case TX_EVENT_FLAG:      return "EVENT_FLAG";
        case TX_BLOCK_MEMORY:    return "BLOCK_MEM";
        case TX_BYTE_MEMORY:     return "BYTE_MEM";
        case TX_IO_DRIVER:       return "IO_DRIVER";
        case TX_FILE:            return "FILE";
        case TX_TCP_IP:          return "TCP_IP";
        case TX_MUTEX_SUSP:      return "MUTEX_SUSP";
        default:                 return "?";
    }
}

static void wd_dump_threads(const char *tag)
{
    printf("\n!! WATCHDOG %s heartbeat=%lu\n", tag, _print_data_count);

    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE

    /* --- snapshot threads --- */
    TX_THREAD *t = _tx_thread_created_ptr;
    ULONG tcount  = _tx_thread_created_count;
    struct { const char *name; UINT state; UINT prio; UINT susp; } tsnap[16];
    ULONG tn = 0;
    for (ULONG i = 0; i < tcount && t && tn < 16; i++) {
        tsnap[tn].name  = t->tx_thread_name;
        tsnap[tn].state = t->tx_thread_state;
        tsnap[tn].prio  = t->tx_thread_priority;
        tsnap[tn].susp  = (UINT)t->tx_thread_suspend_status;
        tn++;
        t = t->tx_thread_created_next;
    }

    /* --- snapshot the IP instance(s) --- */
    NX_IP *ip = _nx_ip_created_ptr;
    ULONG icount = _nx_ip_created_count;
    struct {
        const char *name;
        ULONG pool_avail;
        ULONG pool_total;
        ULONG ip_events_current;       /* pending event-flag mask */
        ULONG tcp_socket_count;
        VOID *driver_deferred_head;    /* WiFi driver pushed → IP not yet pulled */
        VOID *deferred_received_head;  /* IP accepted → protocol not yet processed */
    } isnap[2];
    ULONG in_ = 0;

    /* --- snapshot TCP sockets on the first IP instance --- */
    struct {
        const char *name;
        UINT state;
        ULONG transmit_sent_count;
        VOID *transmit_sent_head;
        ULONG window_advertised;
        ULONG timeout_retries;
    } ssnap[4];
    ULONG sn = 0;

    for (ULONG i = 0; i < icount && ip && in_ < 2; i++) {
        isnap[in_].name                 = ip->nx_ip_name;
        isnap[in_].pool_avail           = ip->nx_ip_default_packet_pool ?
                                          ip->nx_ip_default_packet_pool->nx_packet_pool_available : 0;
        isnap[in_].pool_total           = ip->nx_ip_default_packet_pool ?
                                          ip->nx_ip_default_packet_pool->nx_packet_pool_total : 0;
        isnap[in_].ip_events_current    = ip->nx_ip_events.tx_event_flags_group_current;
        isnap[in_].tcp_socket_count     = ip->nx_ip_tcp_created_sockets_count;
        isnap[in_].driver_deferred_head = (VOID *)ip->nx_ip_driver_deferred_packet_head;
        isnap[in_].deferred_received_head = (VOID *)ip->nx_ip_deferred_received_packet_head;

        /* Walk this IP's TCP sockets. */
        if (in_ == 0) {
            NX_TCP_SOCKET *s = ip->nx_ip_tcp_created_sockets_ptr;
            ULONG sc = ip->nx_ip_tcp_created_sockets_count;
            for (ULONG j = 0; j < sc && s && sn < 4; j++) {
                ssnap[sn].name                = s->nx_tcp_socket_name;
                ssnap[sn].state               = s->nx_tcp_socket_state;
                ssnap[sn].transmit_sent_count = s->nx_tcp_socket_transmit_sent_count;
                ssnap[sn].transmit_sent_head  = (VOID *)s->nx_tcp_socket_transmit_sent_head;
                ssnap[sn].window_advertised   = s->nx_tcp_socket_tx_window_advertised;
                ssnap[sn].timeout_retries     = s->nx_tcp_socket_timeout_retries;
                sn++;
                s = s->nx_tcp_socket_created_next;
            }
        }

        in_++;
        ip = ip->nx_ip_created_next;
    }

    TX_RESTORE

    for (ULONG i = 0; i < tn; i++) {
        printf("  thr %s state=%s prio=%u susp=%u\n",
               tsnap[i].name ? tsnap[i].name : "(null)",
               thread_state_name(tsnap[i].state),
               tsnap[i].prio, tsnap[i].susp);
    }
    for (ULONG i = 0; i < in_; i++) {
        printf("  ip %s pool=%lu/%lu ev_pending=0x%08lx tcp_sockets=%lu\n",
               isnap[i].name ? isnap[i].name : "(null)",
               isnap[i].pool_avail, isnap[i].pool_total,
               isnap[i].ip_events_current, isnap[i].tcp_socket_count);
        printf("  ip %s drv_deferred=%p ip_deferred=%p\n",
               isnap[i].name ? isnap[i].name : "(null)",
               isnap[i].driver_deferred_head,
               isnap[i].deferred_received_head);
    }
    for (ULONG i = 0; i < sn; i++) {
        printf("  tcp %s state=%u tx_sent_count=%lu tx_sent_head=%p"
               " tx_win_adv=%lu retries=%lu\n",
               ssnap[i].name ? ssnap[i].name : "(null)",
               ssnap[i].state,
               ssnap[i].transmit_sent_count,
               ssnap[i].transmit_sent_head,
               ssnap[i].window_advertised,
               ssnap[i].timeout_retries);
    }
}

static void wd_thread_entry(ULONG arg)
{
    (void)arg;
    unsigned long last_count = 0;

    printf("\n[watchdog] thread started\n");

    /* Initial baseline so we don't false-trigger before the first cycle. */
    tx_thread_sleep(WD_INTERVAL_TICKS);
    last_count = _print_data_count;

    for (;;) {
        tx_thread_sleep(WD_INTERVAL_TICKS);
        unsigned long now = _print_data_count;
        /* Only print when the heartbeat actually advances, so the
         * output rate is naturally bounded by the cycle rate (and we
         * don't spam during a wedge).  When wedged the heartbeat
         * stops, so this loop falls silent — caller will see "no
         * watchdog output" which itself is the wedge signal. */
        if (now != last_count) {
            printf("[watchdog] heartbeat=%lu\n", now);
        }
        (void)wd_dump_threads;
        last_count = now;
    }
}

void watchdog_start_once(void)
{
    if (wd_started) return;
    wd_started = 1;
    printf("\n[watchdog] start_once called\n");
    UINT rc = tx_thread_create(&wd_thread, "watchdog",
                               wd_thread_entry, 0,
                               wd_stack, sizeof(wd_stack),
                               WD_PRIORITY, WD_PRIORITY,
                               TX_NO_TIME_SLICE, TX_AUTO_START);
    printf("[watchdog] tx_thread_create rc=%u\n", rc);
}
