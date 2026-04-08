/*
 * Override of the sample's __io_putchar() so printf bypasses HAL
 * entirely and writes directly to USART1 (the ST-Link VCP).
 *
 * Why: HAL_UART_Transmit() is not safe to call from multiple ThreadX
 * threads concurrently — when two threads race the call, one of them
 * silently loses its bytes (HAL returns HAL_BUSY but the lost bytes
 * are not recovered).  In this reproducer the sample's TCP server
 * thread calls printf at ~30 Hz while we also want a low-priority
 * watchdog thread to print thread-state dumps when the system wedges,
 * and the watchdog's prints kept getting eaten.
 *
 * Direct register access is intrinsically safe because USART1->TDR
 * is a single 32-bit write with no software state to corrupt.  We
 * spin-wait on TXE/TXFNF before each write so writes from any thread
 * (or fault context) just queue up at the hardware.
 *
 * Linked first via --allow-multiple-definition so this wins over the
 * sample's main.c implementation.  No ST source files are modified.
 */

#include <stdint.h>
#include "stm32u5xx.h"
#include "tx_api.h"

int __io_putchar(int ch)
{
    /* Make the spin-then-write sequence atomic against ThreadX
     * preemption.  Without this, two threads racing the call can
     * each pass the TXE check, and the second writer's TDR store
     * arrives while the first writer's byte is still in the FIFO,
     * losing one of the bytes. */
    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    while (!(USART1->ISR & USART_ISR_TXE_TXFNF)) { }
    USART1->TDR = (uint8_t)ch;
    TX_RESTORE
    return ch;
}
