/*
 * fault_dump.c -- diagnostic Cortex-M33 fault handlers for the
 * reproducer.  Override the silent infinite loops that ST's sample
 * provides in stm32u5xx_it.c so we can tell failure mode A
 * (BusFault inside _nx_ip_packet_send) apart from failure mode B
 * (silent hang) without modifying any ST source files.
 *
 * The Makefile compiles this file first and links with
 * --allow-multiple-definition, so our HardFault/BusFault/MemManage/
 * UsageFault handlers win over the sample's defaults (and over the
 * ThreadX port's defaults in tx_initialize_low_level.S).
 *
 * On a fault we dump PC, LR, xPSR, CFSR, HFSR, MMFAR and BFAR by
 * poking USART1 directly (the same UART the sample's printf uses)
 * and then halt.  We bypass HAL because the IP/driver state may be
 * corrupted at fault time.
 */

#include <stdint.h>
#include "stm32u5xx.h"

static void fault_putc(char c)
{
    while (!(USART1->ISR & USART_ISR_TXE_TXFNF)) { }
    USART1->TDR = (uint8_t)c;
}

static void fault_puts(const char *s)
{
    while (*s) fault_putc(*s++);
}

static void fault_hex(uint32_t v)
{
    static const char H[] = "0123456789abcdef";
    fault_puts("0x");
    for (int i = 28; i >= 0; i -= 4) fault_putc(H[(v >> i) & 0xF]);
}

void fault_dump(uint32_t *sp, const char *name)
{
    __disable_irq();
    fault_puts("\r\n!! FAULT: ");
    fault_puts(name);
    fault_puts("\r\n  pc=");    fault_hex(sp[6]);
    fault_puts("\r\n  lr=");    fault_hex(sp[5]);
    fault_puts("\r\n  psr=");   fault_hex(sp[7]);
    fault_puts("\r\n  cfsr=");  fault_hex(*(volatile uint32_t *)0xE000ED28);
    fault_puts("\r\n  hfsr=");  fault_hex(*(volatile uint32_t *)0xE000ED2C);
    fault_puts("\r\n  mmfar="); fault_hex(*(volatile uint32_t *)0xE000ED34);
    fault_puts("\r\n  bfar=");  fault_hex(*(volatile uint32_t *)0xE000ED38);
    fault_puts("\r\n");
    while (1) { }
}

#define FAULT_HANDLER(NAME)                          \
__attribute__((naked)) void NAME ## _Handler(void)   \
{                                                    \
    __asm volatile (                                 \
        "tst   lr, #4         \n"                    \
        "ite   eq             \n"                    \
        "mrseq r0, msp        \n"                    \
        "mrsne r0, psp        \n"                    \
        "ldr   r1, =1f        \n"                    \
        "b     fault_dump     \n"                    \
        "1: .asciz \"" #NAME "\" \n"                 \
        ".align 2             \n"                    \
    );                                               \
}

FAULT_HANDLER(HardFault)
FAULT_HANDLER(BusFault)
FAULT_HANDLER(MemManage)
FAULT_HANDLER(UsageFault)
