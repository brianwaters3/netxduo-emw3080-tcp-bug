# NetXDuo + EMW3080 TCP server wedge reproducer

A minimal reproducer for a **rate-dependent wedge** in the EMW3080
WiFi RX path on the B-U585I-IOT02A discovery board, observable through
NetXDuo's TCP server.  Under sustained TCP server traffic at >5
connections/sec, the entire device locks up after ~20 seconds and
stops responding to **all** incoming traffic — TCP, ICMP, everything
— until the board is power-cycled.  At ≤1 connection/sec the same
device runs indefinitely with normal recoverable error rates.

The wedge reproduces against **STM32CubeU5 v1.8.0**'s own unmodified
`Nx_TCP_Echo_Server` sample, so no application-side mistakes can be
blamed.

> **Likely subsystem:** the EMW3080 SPI/RX driver
> (`Drivers/BSP/Components/mx_wifi/` and
> `Middlewares/ST/netxduo/common/drivers/wifi/mxchip/nx_driver_emw3080.c`),
> NOT NetXDuo's TCP layer.  See "Where the bug lives" below.

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

A test client (`client.py`) repeatedly opens a TCP connection, sends
one byte, reads the echo, and closes.  When run at the default **30+
cycles/sec**, the device wedges within ~20 seconds (typically after
100–600 successful cycles).  At **1 cycle/sec** the same device runs
indefinitely with only ~1% recoverable errors and stays responsive.

## Rate dependence (the key finding)

| Client rate | Wall time to wedge | Cycles before wedge | Outcome |
|---|---|---|---|
| **30 cycles/sec** | ~20 sec | 100–2600+ (avg ~600) | Device wedges. Power cycle required. |
| **5 cycles/sec** | ~20 sec | ~100 | Device wedges. Power cycle required. |
| **1 cycle/sec** | **does not wedge** | runs indefinitely | ~1% recoverable RST/timeout (normal WiFi loss) |

The wedge fires after roughly the **same wall time (~20 sec)** at both
30/sec and 5/sec despite the 6× rate difference.  At 1/sec the wedge
does not fire at all in 240+ seconds of testing.  The threshold lies
somewhere between 1 and 5 connections per second.

This pattern strongly suggests **a buffer/queue/state in the EMW3080
SPI driver path that fills up at a rate proportional to sustained
load above some threshold and then locks the receive pipeline**.  At
1/sec the rate is below the threshold and the buffer drains between
cycles; above the threshold it fills and never drains.

## What the wedge looks like

When the device wedges:

- The Python client gives up with `socket.timeout` or
  `ConnectionResetError`
- **Concurrent `ping` requests also stop responding** (verified
  with parallel `ping -i 0.2`)
- The on-device watchdog thread (see "Diagnostics") stops printing
  alive heartbeats — its `_print_data_count` counter freezes
- Watchdog thread-state dumps at the wedge moment show:
  - `App TCP Thread state=TX_TCP_IP` (suspended in NetXDuo internal)
  - `Main Ip instance state=EVENT_FLAG ev_pending=0x0` (idle, no
    events to wake it)
  - `MX_WIFI_TxRxThreadId state=SEMA_SUSP` (waiting for EXTI from
    EMW3080 that never fires)
  - `MX_WIFI_RecvThreadId state=QUEUE_SUSP` (waiting for work from
    SPI thread that never arrives)
  - `pool=9/10` (packet pool healthy — NOT a leak)
  - `drv_deferred=0` and `ip_deferred=0` (no packets queued in either
    NetXDuo deferred queue)
  - **No packets are anywhere in NetXDuo's receive pipeline**.  The
    EMW3080 stopped delivering packets to the SPI driver.

This is not a NetXDuo TCP-state-machine bug — the IP thread is
asleep with literally nothing to do.  The wedge is upstream of
NetXDuo, in the SPI/EXTI path between the EMW3080 module and the
mx_wifi RX thread.

## Where the bug lives (best evidence)

The smoking gun is the simultaneous death of TCP and ICMP combined
with the watchdog dump showing the `mx_wifi` threads sleeping on
their semaphores/queues with no events arriving.  ICMP and TCP go
through the same upstream pipeline:

```
EMW3080 module
    ↓ (SPI + data-ready EXTI)
mx_wifi RX thread (mx_wifi_spi.c)
    ↓
nx_driver_emw3080 RX callback (nx_driver_emw3080.c)
    ↓ (_nx_ip_driver_deferred_receive)
NetXDuo IP thread queue
    ↓ (sets NX_IP_DRIVER_PACKET_EVENT)
_nx_ip_thread_entry
    ↓
_nx_ip_packet_receive → _nx_ipv4_packet_receive → demux by protocol
    ├── ICMP → _nx_icmp_packet_receive  (auto-replies)
    └── TCP  → _nx_tcp_packet_receive → _nx_tcp_packet_process
```

When the wedge fires, **no protocol gets handled**.  The IP thread
is sleeping with no events queued.  That means the wakeup chain
broke at or before "set NX_IP_DRIVER_PACKET_EVENT" — i.e., either
the EMW3080 stopped asserting its data-ready EXTI line, or the
mx_wifi RX/SPI thread stopped reading from the EMW3080, or the
driver received packets but failed to signal the IP thread.

All three live in the EMW3080 driver, NOT in NetXDuo.

## Secondary symptom: NetXDuo accept-retry ISN regeneration

When the device is *almost* wedged (slow but still processing some
packets), a secondary corruption pattern appears in
`nx_tcp_server_socket_accept.c:142-173` that makes the wedge worse
and confuses any client TCP stack:

When `accept()` times out (`NX_NOT_CONNECTED`) because the host's
ACK to the device's SYN-ACK didn't arrive in time, the failure path
at line 199-202 resets state to `NX_TCP_LISTEN_STATE` and clears the
timer, **but leaves `bound_next` and `connect_port` set**.  When the
sample's loop retries `accept()`, the function re-enters its init
block and:

1. Re-randomizes `tx_sequence` (line 113-122 — generates a NEW ISN)
2. Enters the `bound_next` branch (line 142) because `bound_next`
   is still set from the failed attempt
3. Increments `tx_sequence` and emits a new SYN-ACK with the new ISN
4. Returns success-pending; the application loops, the IP thread is
   slow, accept times out again, and the cycle repeats with yet
   another new ISN

The captured `[syn_wrap]` traces in this repo's diagnostic build
show this pattern explicitly: same client port, same SYN seq number,
but the device emitting **multiple SYN-ACKs with different ISNs**
(e.g. `tx_seq=1389254635 → tx_seq=3299820847 → tx_seq=175889579 →
peer=0.0.0.0:0 tx_seq=2032179306`), the last one with a corrupted
zeroed connect tuple.  After this the IP thread wedges entirely.

This is a **secondary** NetXDuo bug that is exposed by the slow IP
thread under load, not the root cause.  Fixing it would not stop
the wedge from firing — it would only make the failure mode less
catastrophic.

### Failure mode A: BusFault inside `_nx_ip_packet_send` (historical)

Originally reported as a primary failure mode but **not observed
once diagnostic fault handlers were installed** (zero BusFaults
across 20+ runs).  Most likely a previous unrelated artifact.  Kept
here for reference.

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
cycles the IP/TCP stack stops making forward progress.  No fault is
raised.  The packet pool stays at its normal `46/48` available — so
this is **not** packet-pool exhaustion.

The Python client gives up first, with one of:

* `socket.timeout` (the server stops sending the echo / stops accepting)
* `ConnectionResetError` (the server tears the socket down without
  shutting down the listener)

Mode B is what we observe overwhelmingly often once a HardFault dump
handler is wired in (see "Diagnostics" below): in 20 consecutive runs
against the unmodified sample we got mode B every time and mode A
zero times.  This suggests mode B is the dominant signature on this
firmware/board combination, and that mode A as observed in our own
application is triggered by app-side timing differences (different
thread priorities, additional threads competing for the IP mutex,
different traffic mix) rather than by the TCP echo workload alone.

#### Slow drift before the hang

Watching the green LED (which the sample toggles before and after
each `nx_tcp_socket_send`) makes the failure surprisingly visible:

* For the first few hundred cycles the LED *flickers* — `send`
  returns in milliseconds, so it spends almost no time between toggles.
* Over hundreds to thousands of cycles the flicker progressively
  smooths into the LED looking more "on than off".  The send latency
  is growing.
* Eventually the LED is either stuck **on** (thread wedged inside
  `nx_tcp_socket_send`, between the toggle on `app_netxduo.c:344` and
  the toggle on `app_netxduo.c:360`) or stuck **off** (thread wedged
  inside `nx_tcp_socket_receive` or `nx_tcp_server_socket_accept`).

This monotone slowdown is consistent with a slow leak / queue-walk
hypothesis: each cycle leaves a packet on
`nx_tcp_socket_transmit_sent_head` that never gets released.  As the
queue grows, the IP thread spends more and more time walking it on
its periodic timer and on each new send, until something deeper
wedges (likely an internal mutex or a corrupted queue link).

## Workaround

**For applications that can tolerate it: rate-limit TCP server
accept rate to ≤1 connection/sec.** At that rate the device runs
indefinitely with normal recoverable error rates and the wedge
never fires.

For applications that need higher rates (real TCP servers): there is
no application-level workaround.  The fix has to land inside ST's
EMW3080 driver.

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

**At default rate (~30 cycles/sec)**, 20 consecutive runs (each
starting from a power-on reset) all wedged with cycle counts
ranging from 15 to 2618 (mean ~702).

**At 1 cycle/sec**: 177 successful cycles in 180 seconds with 3
recoverable errors, then *another* 60 successful cycles in 60
seconds without a power cycle.  The device is completely healthy
at this rate.

In all runs the diagnostic fault handlers (see below) stayed
silent — no HardFault, no BusFault, no MemManage, no UsageFault.
The wedge is purely a "no packets ever reach the IP thread again"
condition, not a fault.

### Diagnostics

This repo's diagnostic build provides several pieces of
instrumentation, all linked via `--allow-multiple-definition` so
they win over ST's defaults without modifying any ST source.

#### `src/fault_dump.c`
Naked `HardFault_Handler`, `BusFault_Handler`, `MemManage_Handler`,
and `UsageFault_Handler` implementations that snapshot the
exception frame and write `pc / lr / psr / cfsr / hfsr / mmfar /
bfar` directly to USART1 before halting.  ST's `stm32u5xx_it.c`
also defines these as silent infinite loops; we override them.

#### `src/io_putchar.c`
Override of `__io_putchar` that bypasses HAL and writes directly
to `USART1->TDR` with `TX_DISABLE`/`TX_RESTORE` around the
spin-then-write.  HAL_UART_Transmit is not multi-thread-safe and
loses bytes when two threads call printf concurrently — this
override fixes that, which is important because we have a
watchdog thread and the TCP thread both printing.

#### `src/watchdog.c`
A low-priority diagnostic thread that wakes once per cycle (only
when the heartbeat counter advances), prints `[watchdog]
heartbeat=N`, and would dump full thread/socket state if asked.
The PRINT_DATA macro override in `inc/app_netxduo.h` increments
the counter and starts the watchdog on the first cycle.  When
the device wedges, the heartbeat stops advancing and the thread
falls silent — that silence is the wedge signal.

#### `src/syn_wrap.c`
Linker `--wrap` instrumentation for several NetXDuo entry points:

* `_nx_tcp_packet_process` — logs every incoming TCP packet
  (filtered to SYN-only) so you can see what packets reach
  NetXDuo's TCP layer.
* `_nx_tcp_packet_send_syn` — logs every outgoing SYN/SYN-ACK so
  you can see the device's responses.
* `_nx_tcp_server_socket_accept` — logs entry and exit, including
  `bound_next` and `connect_port` state, so you can see the
  accept-retry ISN regeneration pattern when it fires.

#### `src/nx_tcp_fast_periodic_processing_local.c`
A local fork of NetXDuo's `nx_tcp_fast_periodic_processing.c` with
the eclipse-threadx/netxduo#306 workaround applied (the upstream
condition that gates `_nx_tcp_socket_connection_reset` is "never
fulfilled" in normal flow per #306; this fork removes the gate).
Did not affect the wedge in our testing — kept here in case it
matters for related issues.

### What the diagnostic output looks like

A successful cycle:

```
[tcp_in] pkt=0x... sp=64512 dp=6000 flags=S
[accept_in] sock=0x... state=2 bound_next=0x... connect=192.168.0.10:64512
[syn_wrap] send_syn sock=0x... state=4 peer=192.168.0.10:64512 tx_seq=...
[accept_out] sock=0x... rc=0 state=5 connect_port=64512
[watchdog] heartbeat=N
```

The accept-retry ISN regeneration cascade (secondary symptom
described above) looks like:

```
[tcp_in] sp=54693 flags=S
[accept_in] state=2 bound_next=... connect=192.168.0.10:54693
[syn_wrap] tx_seq=1389254635           <- ISN A
[syn_wrap] tx_seq=1389254635           <- retransmit (correct)
[accept_out] rc=56 connect_port=54693  <- NX_NOT_CONNECTED (timeout)
[accept_in] state=2 bound_next=... connect=192.168.0.10:54693
[syn_wrap] tx_seq=3299820847           <- NEW ISN B!
[accept_out] rc=56 connect_port=54693
[accept_in] state=2 bound_next=... connect=192.168.0.10:54693
[syn_wrap] tx_seq=175889579            <- NEW ISN C!
[accept_out] rc=56 connect_port=0      <- connect_port now 0!
[accept_in] state=2 bound_next=... connect=0.0.0.0:0
[syn_wrap] peer=0.0.0.0:0 tx_seq=...   <- SYN-ACK to NOWHERE
(then no further output — wedged forever)
```

Per-cycle prints can be disabled by setting `VERBOSE_LOGS=0` in
`src/syn_wrap.c` (default 0) and the PRINT_DATA macro override in
`inc/app_netxduo.h`, leaving only the watchdog heartbeat.

### Run the client

On any host on the same WiFi network:

```sh
python3 client.py <mcu-ip> 6000           # default ~30 cycles/sec — wedges in ~20 sec
```

The client prints `N cycles ok` every 10 successful round trips.
With the default rate, expect a wedge within ~20 seconds.

To verify the rate-dependence finding (no wedge at low rate), use
the optional included throttled client:

```sh
# Hammer at 1 cycle/sec — should run indefinitely with a few
# recoverable errors but no wedge
python3 client.py <mcu-ip> 6000 --rate 1   # if not yet implemented, edit client.py
```

To verify the wedge takes down ICMP too, run a parallel ping:

```sh
ping -i 0.2 192.168.0.25 &
python3 client.py 192.168.0.25 6000
```

You'll see ping responding normally up to the moment of the wedge,
then **all** ping responses stop at the same instant the TCP
client fails.

## Files in this repo

* `README.md` — this file.
* `Makefile` — builds the unmodified ST sample with `arm-none-eabi-gcc`.
* `client.py` — minimal Python client that reproduces the failure
  at the default ~30 cycles/sec rate.
* `wedge-port65297.pcap` — saved host-side packet capture of one
  failing cycle, showing the host transmitting 4 SYN retransmits at
  1-second intervals with zero device response.
* `src/fault_dump.c` — diagnostic Cortex-M33 fault handlers
  (HardFault / BusFault / MemManage / UsageFault).
* `src/io_putchar.c` — multi-thread-safe `__io_putchar` override
  that bypasses HAL and writes directly to `USART1->TDR`.
* `src/watchdog.c` — low-priority diagnostic thread that prints a
  heartbeat per cycle and falls silent at the wedge moment.
* `src/syn_wrap.c` — linker `--wrap` instrumentation for
  `_nx_tcp_packet_process`, `_nx_tcp_packet_send_syn`, and
  `_nx_tcp_server_socket_accept`.
* `src/nx_tcp_fast_periodic_processing_local.c` — local fork with
  the eclipse-threadx/netxduo#306 workaround.
* `src/nx_tcp_server_socket_accept_local.c` — local fork of
  NetXDuo's accept() with experimental cleanup-on-failure code that
  did NOT fix the wedge (kept here as documentation of one failed
  fix attempt; the bug is upstream of NetXDuo).
* `inc/app_netxduo.h` — local header that pre-defines the sample's
  include guard so we can override `PRINT_DATA` (which we use to
  drive the watchdog heartbeat counter) without modifying ST source.
* `inc/mx_wifi_conf.h` — copy of ST's `mx_wifi_conf.h` patched only
  to `#include "wifi_secrets.h"` if present (so credentials live
  outside source control).
* `inc/wifi_secrets.h.example` — template for the gitignored
  credentials file.
* `.gitignore` — keeps `build/` and `inc/wifi_secrets.h` out of git.
