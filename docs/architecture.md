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

- `firmware`: versioned `c906-mcu.{elf,bin}` artifacts, the page layout they
  were built against (`guidance_contract.json`), and sha256 pins for all three.
- `lib`: gone. The contract JSON is the only copy of the layout; it is installed
  to `/lib/firmware/guidance_contract.json` (next to the ELF it describes) and read
directly by `cop` and
  `bootlogd` (no separate loader module).
- `ports/osdrv/patches`: the CMDQU `IP_SYSTEM` doorbell kernel patch.
- `tools`: `rtos-mode` (boot the C906L remoteproc) and `cop` (the generic page
  tool: status/dump/log/watch/format/send, driven by the contract or by the
  page's own header).
- `linux/systemd`: the `load-systemko` drop-in that starts the C906L before
  the media chain.

The image layer verifies the artifacts, installs the ELF to `/lib/firmware`,
injects the BIN into the FSBL/FIP, and installs the tools and drop-in.
Firmware changes are refreshed from the product repository and re-pinned in
`firmware/sha256sums.txt`; image builds never compile the firmware.

The boot policy is a board setting, not a build accident: `configs/<board>/settings.mk`
declares who releases the C906L, whether `cvitek_remoteproc` loads the firmware or
attaches to an already-running core, and who owns the doorbell IRQ. `scripts/Makefile`
derives the FSBL input and generates the systemd drop-in from it, and
`scripts/ci/validate.py` refuses unsupported combinations and checks the policy
against the device tree.

The page layout itself is not re-derived here: `guidance_contract.json` is the
owner's statement of it, `cop` and `bootlogd` read it directly, and
`scripts/ci/validate.py` fails the build unless
the DTS `guidance-pages` reg and `configs/maixcam/memmap.py` agree with it. The
one fact the two sides must agree on -- the two page addresses -- is declared
once on each side and checked, never copied again.

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
