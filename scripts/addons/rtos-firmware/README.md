# SG2002 RTOS communication stack

This addon provides a fixed-slot shared-memory channel while retaining the
vendor 8-byte CMDQU ABI as its doorbell and compatibility fallback.

## Ownership

- `include/sg2002_rtos_shm.h`: transport ABI, control page, slot layout, and
  generic Linux ioctls.
- `include/sg2002_rtos_ring.h` and `common/sg2002_rtos_ring.c`: platform-neutral
  SPSC publication and return logic.
- `include/sg2002_rtos_protocol.h`: application commands and message framing.
- `linux/`: the main-core library. Applications call `sg2002_rtos_*`; they do
  not construct `cmdqu_t` or manipulate shared counters.
- `configs/common/patches/osdrv/0006-*`: a self-contained Linux shared-memory
  driver. The vendor CMDQU file only forwards lifecycle, ioctl, poll, and
  doorbell events.
- `freertos/src/sg2002_rtos_mailbox.c`: mailbox MMIO, receiver interrupt
  state, hardware spinlock, and Linux/RTOS direction bits only.
- `freertos/src/sg2002_rtos_shm_transport.c`: C906L cache maintenance,
  generation attachment, and queue draining.
- `freertos/src/sg2002_rtos_app.c`: C906L application command handlers.
- `tools/`: thin examples and diagnostics built on the Linux library.

To add an application command, assign it in the protocol header, add its
handler to `sg2002_rtos_app.c`, and optionally add a typed Linux wrapper.
Kernel, ring, cache, and mailbox code remain unchanged while each request and
response fits in one 506-byte transport payload.

## Shared-memory contract

The 68 KiB region is `4 KiB control + 64 * 512 B requests + 64 * 512 B
responses`. Every slot is exactly:

```c
struct sg2002_rtos_shm_slot {
    uint32_t sequence;
    uint16_t length;
    uint8_t payload[506];
};
```

The Linux kernel is the sole writer of the request producer and response
consumer counters. C906L is the sole writer of the request consumer and
response producer counters. Each counter and each peer state occupies its own
64-byte cache line. Advancing a consumer counter returns that slot; there is no
separate free-block queue.

Linux accesses the reserved DDR through an uncached I/O mapping. C906L
invalidates before reading remote publications and cleans the complete slot
before publishing its producer counter. Release/acquire barriers enforce
`slot -> producer` and `slot read -> consumer` ordering. A Linux-owned
generation and two separate peer-state lines make driver or C906L restarts
explicit. CMDQU command `0x7f` is permanently reserved as the transport
doorbell.

`sg2002_rtos_open()` attempts to acquire this channel. Existing 32-bit typed
calls use it when available and fall back to legacy CMDQU on an old driver or
busy/unavailable C906L. Generic `sg2002_rtos_message_send()`,
`sg2002_rtos_message_receive()`, and `sg2002_rtos_message_call()` require the
shared channel and preserve sequence IDs across multiple outstanding requests.

The C906L receive path is interrupt-driven. Firmware explicitly unmasks the
CPU2 mailbox channels during startup, the ISR copies at most eight published
slots into FreeRTOS queues, and the CMDQU task blocks between commands. A
100 ms scan remains only as missed-interrupt recovery; `boot_trace` reports
separate IRQ and recovery counters plus mailbox, PLIC, and CSR state.

## Build and test

The integrated image build invokes `addon.mk`. The C906L firmware can also be
built on its own from the repository root:

```sh
OUT_DIR="$PWD/rtos_out" bash scripts/build_rtos_only.sh
```

Run native protocol/application/library tests with:

```sh
CC=cc bash scripts/addons/rtos-firmware/tests/run_host_tests.sh \
  /path/to/duo-buildroot-sdk-v2/cvi_mpi/include /tmp/sg2002-tests
```

On target, `rtos-bench [samples [warmup [echo-data-bytes]]]` measures the
complete synchronous userspace-to-FreeRTOS round trip. With no payload argument
it benchmarks PING over the selected transport. Values `1..505` use the
application ECHO command and report both fixed 512-byte slot traffic and useful
application bytes. The host test suite covers ABI size, full/empty behavior,
32-bit counter wraparound, generation reattachment, application dispatch,
Linux shared-memory/CMDQU fallback behavior, and dependency guards for the
layer boundaries above.
