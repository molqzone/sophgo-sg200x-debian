# Repository architecture

## Board resolution

`scripts/ci/plan.py` is the only board resolver used by Make, local builds, and
CI. `configs/maixcam/board.mk` declares:

```make
BASE_BOARD := licheervnano
```

Single files and owned directories such as `dts/` use this precedence:

```text
maixcam -> licheervnano -> common
```

An exact owned directory replaces the inherited directory rather than merging
unrelated board files into it. Component-specific platform aliases also belong
in board settings; for example, `FSBL_CHIP` defaults to `CHIP`, while
licheervnano maps its `sg200x` board identifier to the FSBL tree's `cv181x`
platform directory.

Settings and patches apply in the opposite direction so the exact board has
the final override:

```text
common -> licheervnano -> maixcam
```

Patch identity is its filename. A higher-priority layer can replace an
inherited patch with the same filename or suppress it with
`<name>.patch.skip`. Board-specific decisions belong in settings or the matrix,
not in workflow conditionals.

## C906L firmware ownership

`components/guidance-fw` owns the product firmware for the small core:

- `firmware`: versioned `c906-mcu.{elf,bin}` artifacts with sha256 pins.
- `ports/osdrv/patches`: the CMDQU `IP_SYSTEM` doorbell kernel patch.
- `tools`: `rtos-mode` (boot the C906L remoteproc) and `guidance-cmd`
  (ping/led commands over the doorbell).
- `linux/systemd`: the `load-systemko` drop-in that starts the C906L before
  the media chain.

The image layer verifies the artifacts, installs the ELF to `/lib/firmware`,
injects the BIN into the FSBL/FIP, and installs the tools and drop-in.
Firmware changes are refreshed from the product repository and re-pinned in
`firmware/sha256sums.txt`; image builds never compile the firmware.

## Build ownership

Version sources are deliberately separate:

- `toolchain.env`: Docker base snapshot and host cross-toolchain commit.
- `versions.env`: Linux, boot, multimedia, firmware, and package source pins.
- `components/guidance-fw/firmware/sha256sums.txt`: C906L firmware artifact pins.

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
