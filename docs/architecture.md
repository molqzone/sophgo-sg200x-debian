# Repository architecture

## Board resolution

`scripts/ci/plan.py` is the only board resolver used by Make, local builds, and
CI. `configs/maixcam/board.mk` declares:

```make
BASE_BOARD := licheervnano
```

Single files use this precedence:

```text
maixcam -> licheervnano -> common
```

Settings and patches apply in the opposite direction so the exact board has
the final override:

```text
common -> licheervnano -> maixcam
```

Patch identity is its filename. A higher-priority layer can replace an
inherited patch with the same filename or suppress it with
`<name>.patch.skip`. Board-specific decisions belong in settings or the matrix,
not in workflow conditionals.

## SG2002 IPC ownership

`components/sg2002-ipc` owns the complete communication contract:

- `include`: stable protocol, 1 KiB shared-memory slot, and SPSC ring ABI.
- `core`: platform-neutral ring implementation.
- `firmware/app`: application commands and handlers.
- `firmware/transport`: mailbox and shared-memory transport.
- `firmware/platform/sg2002`: SoC boundary.
- `linux`: main-core library and tools.
- `ports/duo-sdk`: pinned SDK overlay and firmware builder.
- `ports/osdrv`: Linux kernel integration patches and vendor ABI headers.
- `tests`: host-side ABI, queue, transport, application, and library tests.

The image layer only invokes the component Makefile and installs its outputs.
Adding an application command should normally touch `firmware/app`, the public
protocol header, and an optional Linux wrapper. It must not require edits to
the vendor SDK overlay, OSdrv, mailbox, cache, or queue implementation.

## Build ownership

Version sources are deliberately separate:

- `toolchain.env`: Docker base snapshot and host cross-toolchain commit.
- `versions.env`: Linux, boot, multimedia, firmware, and package source pins.
- `components/sg2002-ipc/versions.env`: C906L vendor SDK pin.

The Docker image contains tools only. The active checkout mounts as
`/workspace`, `scripts` as `/builder`, and `configs` as `/configs`. Therefore a
branch never needs a branch-specific Docker tag and CI never runs stale scripts
baked into an image.

## CI responsibilities

The local runner provides the fast persistent development loop. CI always uses
an empty container build tree:

```text
ci.yml       host tests -> C906L -> Linux tools -> affected clean board builds
images.yml   full clean matrix -> format-aware packaging -> optional release
toolchain.yml immutable toolchain image, only on toolchain input changes
```

`configs/build-matrix.json` is the only list of supported board/storage/output
combinations. Both workflows consume JSON emitted by `plan.py`.
