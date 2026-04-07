# NetXDuo + EMW3080 TCP server bug reproducer

A minimal reproducer for two related failure modes in the NetXDuo TCP
server path on the B-U585I-IOT02A discovery board (EMW3080 WiFi over
SPI2, NetXDuo as the IP stack).

The bug reproduces against **STM32CubeU5 v1.8.0**'s own unmodified
`Nx_TCP_Echo_Server` sample, so no application-side mistakes can be
blamed.

## Hardware / software

| Component | Version |
|-----------|---------|
| Board | STMicro B-U585I-IOT02A |
| MCU | STM32U585AI (Cortex-M33) |
| WiFi module | EMW3080 (SPI2) |
| EMW3080 firmware | V2.3.4 |
| STM32CubeU5 | 1.8.0 (FW.U5.1.8.0) |
| ThreadX | as bundled in CubeU5 1.8.0 |
| NetXDuo | as bundled in CubeU5 1.8.0 |
| Toolchain | arm-none-eabi-gcc 15.2.1 |

## What goes wrong

The MCU runs a NetXDuo TCP echo server on port 6000 using the standard
`nx_tcp_server_socket_listen` / `accept` / `disconnect` / `unaccept` /
`relisten` pattern (the exact pattern shown in ST's own sample, see
`Nx_TCP_Echo_Server/NetXDuo/App/app_netxduo.c` lines 322–374).

A test client (this directory's `client.py`) repeatedly connects, sends
one byte, reads the echo, and disconnects.  After **5–80 successful
cycles** — the count varies run to run — one of two things happens:

### Failure mode A: BusFault inside `_nx_ip_packet_send`

```
!! FAULT: BusFault
  pc=0x0800cf92        <-- nx_ip_packet_send.c line 216
  lr=0x08010acf        <-- nx_tcp_socket_retransmit.c line 404
  psr=0x21000000
  cfsr=0x00008200      <-- BFARVALID + PRECISERR
  bfar=0x000c7e94      <-- always a small low-memory address
```

`addr2line` resolves `pc` to:

```c
/* nx_ip_packet_send.c:216 */
_nx_ip_header_add(ip_ptr, packet_ptr,
                  packet_ptr->nx_packet_ip_interface->nx_interface_ip_address,
                  destination_ip, type_of_service, time_to_live,
                  protocol, fragment);
```

The fault is on the second dereference: `packet_ptr->nx_packet_ip_interface`
is non-`NULL` (so the check at line 118 of the same function passes),
but it points to a low-memory garbage value (always in the `0x000xxxxx`
range — `0x00153e80`, `0x000c7e80`, `0x00457ef8`, `0x000d7ef8`, etc.).
Subtracting the offset of `nx_interface_ip_address` (0x14) gives the
faulting interface pointer; the high byte being `0x00` is consistent
across many runs, suggesting one byte of a valid `0x20xxxxxx` RAM
pointer is being clobbered to zero — possibly by a misaligned write
or a stale write into recycled packet memory.

The call stack is `_nx_tcp_socket_retransmit` → `_nx_ip_packet_send`,
i.e. NetXDuo's TCP retransmit timer fires for a queued packet whose
`nx_packet_ip_interface` field has been corrupted since the original
send.

### Failure mode B: silent hang

Other runs hit a quieter failure: after some number of successful
cycles the MCU's control thread is alive (the LED keeps blinking,
the ST-Link console is responsive) but `nx_tcp_server_socket_accept`
stops returning.  New TCP SYNs from the client time out.  No fault is
raised.  The packet pool stays at its normal `46/48` available — so
this is **not** packet-pool exhaustion.

We have not been able to pin down where exactly the IP stack wedges
in this mode without instrumentation that itself perturbs the timing
enough to change the failure rate.  Both modes are reproducible against
the same unmodified sample, so we believe they are two faces of the
same underlying state corruption.

## Why we think it's NetXDuo, not the application

* The application code is ST's own sample, byte-for-byte unchanged.
* The pattern (single server socket reused via `relisten`) is the
  documented NetXDuo idiom.
* We verified that `nx_packet_pool_available` stays high, so this
  isn't a leak.
* In failure mode A, the fault is *inside* NetXDuo, on a pointer that
  NetXDuo itself owns (`packet_ptr->nx_packet_ip_interface` is set
  by NetXDuo's own TCP send path, not by the driver).

## Why we think the EMW3080 NetXDuo driver is involved

`Middlewares/ST/netxduo/common/drivers/wifi/mxchip/nx_driver_emw3080.c`,
function `_nx_driver_emw3080_packet_send`, calls
`nx_packet_transmit_release(packet_ptr)` immediately after the
synchronous SPI write returns.  For TCP packets,
`nx_packet_transmit_release` does *not* free the packet — it leaves
the packet on the socket's `nx_tcp_socket_transmit_sent_head` queue
with `nx_packet_queue_next == NX_DRIVER_TX_DONE`, expecting the
packet's other fields (including `nx_packet_ip_interface`) to remain
valid until the TCP layer ACKs and releases it.

That contract appears to break under load on this driver — by the time
the retransmit timer fires later, the field has been overwritten.  We
have **not** identified the writer.  Possibilities we considered but
could not confirm:

1. The mx_wifi RX path recycling the same NX_PACKET memory (unlikely;
   the packet is still on a queue and shouldn't be in the free list).
2. NetXDuo's own internal release path being triggered prematurely
   somewhere.
3. A union-aliasing issue inside NetXDuo where an IPv6 code path
   writes through `nx_packet_address.nx_packet_ipv6_address_ptr`
   for an IPv4 packet.

## Reproduction steps

This repo's `Makefile` builds STM32CubeU5's `Nx_TCP_Echo_Server` sample
**byte-for-byte unmodified** with `arm-none-eabi-gcc`.  The only thing
this repo provides on top of the ST tree is:

* a higher-priority include path for `mx_wifi_conf.h` (so we can supply
  real WiFi credentials via a gitignored `inc/wifi_secrets.h` instead of
  hand-editing the sample), and
* two linker `--defsym` flags so the IAR-style symbols
  `__RAM_segment_used_end__` and `_vectors` resolve under GNU `ld`.

No source files in the ST tree are modified.

### Quickstart

```sh
# 1. Point at your STM32CubeU5 install (default: ~/code/stm32/STM32CubeU5)
export STM32CUBE_DIR=/path/to/STM32CubeU5

# 2. Provide WiFi credentials
cp inc/wifi_secrets.h.example inc/wifi_secrets.h
$EDITOR inc/wifi_secrets.h          # set WIFI_SSID / WIFI_PASSWORD

# 3. Build, flash, hammer it
make
make flash
# (open a serial terminal at 115200 8N1 to read the assigned IP)
python3 client.py <mcu-ip> 6000
```

### Confirmed reproduction

Reproduced against ST's unmodified sample on:

* B-U585I-IOT02A + EMW3080 V2.3.4
* STM32CubeU5 1.8.0
* arm-none-eabi-gcc 15.2.1

A typical run hits one of the failure modes within 5–80 client cycles.
On one recorded run we got `ConnectionResetError` after 35 cycles
(failure mode B — silent hang, no fault dump).

### Add a HardFault diagnostic (optional, for failure mode A)

By default ST's `HardFault_Handler` is a silent infinite loop, which
makes failure mode A invisible.  Replace it with something that prints
the stacked PC/LR/CFSR/BFAR before halting — for example:

```c
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile (
        "tst   lr, #4         \n"
        "ite   eq             \n"
        "mrseq r0, msp        \n"
        "mrsne r0, psp        \n"
        "ldr   r1, =1f        \n"
        "b     fault_dump     \n"
        "1: .asciz \"HardFault\" \n"
        ".align 2             \n"
    );
}
```

(plus a `fault_dump(uint32_t *sp, const char *name)` that pokes USART1
directly).  Without this, failure mode A looks identical to failure
mode B from the outside.

### Run the client

On any host on the same WiFi network:

```sh
python3 client.py <mcu-ip> 6000
```

The client prints `N cycles ok` every 10 successful round trips.
Expected behavior: it runs forever.  Observed behavior: it runs for
a few seconds to a minute and then fails with `socket.timeout`,
`ConnectionResetError`, or `ConnectionRefusedError`.

## Workaround we are using

We patched `_nx_driver_emw3080_packet_send` to pin
`packet_ptr->nx_packet_ip_interface` to `&ip->nx_ip_interface[0]`
right before calling `nx_packet_transmit_release`.  This is a band-aid
— it converts failure mode A (BusFault) into a cleaner code path
(retransmit dereferences a valid interface struct), but it does **not**
fix failure mode B.

```c
/* In _nx_driver_emw3080_packet_send, just before nx_packet_transmit_release */
packet_ptr->nx_packet_ip_interface =
    &(nx_driver_information.nx_driver_information_ip_ptr->nx_ip_interface[0]);
```

After this patch the system survives ~50–100 cycles instead of 5–8,
which is enough to be useful for development but not production.

## Files in this repo

* `README.md` — this file.
* `Makefile` — builds the unmodified ST sample with `arm-none-eabi-gcc`.
* `client.py` — minimal Python client that reproduces the failure.
* `inc/mx_wifi_conf.h` — copy of ST's `mx_wifi_conf.h` patched only to
  `#include "wifi_secrets.h"` if present (so credentials live outside
  source control).
* `inc/wifi_secrets.h.example` — template for the gitignored
  credentials file.
* `.gitignore` — keeps `build/` and `inc/wifi_secrets.h` out of git.
