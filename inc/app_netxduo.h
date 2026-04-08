/*
 * Local override of ST's NetXDuo/App/app_netxduo.h.
 *
 * The Makefile injects this file via -include before each translation
 * unit, and we pre-define the include guard so the sample's own copy
 * becomes a no-op when its sources do `#include "app_netxduo.h"`.  We
 * then provide a drop-in equivalent of the constants and prototypes
 * the sample's header normally exports, with one change: PRINT_DATA
 * is rewritten to a single-line in-place updating heartbeat
 * (`\r<count> <data>`) so the console isn't drowned in per-cycle spam.
 *
 * No ST source files are modified.
 */

#ifndef __APP_NETXDUO_H__
#define __APP_NETXDUO_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "nx_api.h"
#include <stdio.h>
#include "main.h"
#include "nxd_dhcp_client.h"
#include "nx_driver_emw3080.h"

#define PAYLOAD_SIZE             1544
#define NX_PACKET_POOL_SIZE      (( PAYLOAD_SIZE + sizeof(NX_PACKET)) * 10)

#define DEFAULT_MEMORY_SIZE      1024
#define DEFAULT_PRIORITY         10
#define WINDOW_SIZE              512

#define NULL_ADDRESS             0

#define DEFAULT_PORT             6000
#define MAX_TCP_CLIENTS          1

UINT MX_NetXDuo_Init(VOID *memory_ptr);

#define PRINT_IP_ADDRESS(addr)             do { \
                                                printf("STM32 %s: %lu.%lu.%lu.%lu \n", #addr, \
                                                (addr >> 24) & 0xff, \
                                                (addr >> 16) & 0xff, \
                                                (addr >> 8) & 0xff, \
                                                addr& 0xff);\
                                           }while(0)

/* Heartbeat counter shared with src/watchdog.c (defined there). */
extern unsigned long _print_data_count;
extern void watchdog_start_once(void);

/* Local override: replace the per-cycle "[ip:port] -> 'a'" spam with a
 * shorter "<count> <data>\n" line and start the diagnostic watchdog
 * thread on the first cycle.  When the watchdog detects that this
 * counter has stopped advancing, it dumps every ThreadX thread's
 * name and blocked-on state to USART1. */
#define PRINT_DATA(addr, port, data)       do { \
                                                _print_data_count++; \
                                                if (_print_data_count == 1) watchdog_start_once(); \
                                                /* per-cycle print disabled to reduce UART load */ \
                                           } while (0)

#ifdef __cplusplus
}
#endif
#endif /* __APP_NETXDUO_H__ */
