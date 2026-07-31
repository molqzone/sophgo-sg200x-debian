# SG2002 RTOS communication stack

This addon keeps the vendor 8-byte CMDQU ABI while separating application
commands from the Linux and FreeRTOS transports.

## Ownership

- `include/sg2002_rtos_protocol.h`: shared command IDs and 32-bit payload layout.
- `linux/`: the main-core library. Applications call `sg2002_rtos_*` and never
  construct `cmdqu_t` or issue CMDQU ioctls directly.
- `freertos/src/sg2002_rtos_mailbox.c`: mailbox MMIO, receiver interrupt
  state, hardware spinlock, and Linux/RTOS direction bits only.
- `freertos/src/sg2002_rtos_app.c`: C906L application command handlers.
- `tools/`: thin examples and diagnostics built on the Linux library.

To add an application command, assign it in the shared protocol header, add
its handler to `sg2002_rtos_app.c`, and expose an optional typed wrapper in the
Linux library. Kernel and mailbox transport changes are unnecessary while the
request and response each fit in one 32-bit value.

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

On target, `rtos-bench [samples [warmup]]` measures the complete synchronous
userspace-to-FreeRTOS round trip. CMDQU has no transaction ID, so the kernel
serializes synchronous requests. The reported payload and wire rates describe
this control channel; they are not shared-memory bulk-transfer bandwidth.
