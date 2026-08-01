# SG2002 IPC component

This component provides an interrupt-driven fixed-slot shared-memory channel
between Linux and the SG2002 C906L FreeRTOS core. The vendor 8-byte CMDQU ABI is
retained as the doorbell and compatibility fallback.

## Stable transport contract

The active region is `4 KiB control + 64 * 1024 B requests + 64 * 1024 B
responses` (`0x21000` bytes). Every slot is exactly 1024 bytes:

```c
struct sg2002_rtos_shm_slot {
    uint32_t sequence;
    uint16_t length;
    uint8_t payload[1018];
};
```

Linux alone writes the request producer and response consumer counters. C906L
alone writes the request consumer and response producer counters. Advancing a
consumer counter returns the slot, so no free-block queue is required. Each
counter and peer state occupies its own cache line.

Linux uses an uncached I/O mapping. C906L invalidates before reading peer
publication and cleans a full slot before publishing its producer counter.
Release/acquire barriers enforce slot-before-counter ordering. CMDQU command
`0x7f` is reserved as the shared-memory doorbell.

The receive path is interrupt driven. Firmware unmasks the CPU2 mailbox during
startup, the ISR transfers bounded work to FreeRTOS queues, and the worker task
blocks between events. A slow recovery scan remains only for missed-interrupt
diagnostics.

## Application boundary

Application commands belong in:

```text
include/sg2002_rtos_protocol.h
firmware/app/
linux/                         optional typed Linux wrapper
```

Do not construct vendor `cmdqu_t` objects in applications or manipulate shared
counters directly. Transport, mailbox, cache, ring, SDK port, and OSdrv code
should remain unchanged for a request/response that fits the 1018-byte payload.

## Standalone build

From this directory, with the SG2002 cross-toolchains available:

```sh
make test
make firmware BOARD=maixcam CONFIG_ROOT=../../configs
make linux-tools
make verify
```

From the repository root, the persistent local runner provides the toolchain:

```sh
make test
make firmware BOARD=maixcam
```

On Windows use `scripts/ci/local-build.ps1`. The standalone firmware output
includes ELF, BIN, linker-layout JSON, SDK commit, and SHA-256 manifests.

## Linux API

`sg2002_rtos_open()` acquires the shared channel. Existing typed 32-bit calls
fall back to legacy CMDQU when the shared driver is unavailable. Generic
`sg2002_rtos_message_send()`, `sg2002_rtos_message_receive()`, and
`sg2002_rtos_message_call()` require shared memory and preserve sequence IDs
across outstanding requests.

On target, `rtos-bench [samples [warmup [echo-data-bytes]]]` measures complete
userspace-to-FreeRTOS round trips. Payload sizes `1..1017` use the ECHO command;
the report separates fixed 1024-byte slot traffic from useful bytes.
