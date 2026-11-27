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

`components/rtos-fw` owns the product firmware for the small core:

- `firmware`: versioned `c906-mcu.{elf,bin}` artifacts and sha256 pins for both.
- `lib`: gone, and so is the contract JSON. The page layout is owned by the
  firmware's C++ headers and is self-described on the page; `cop` reads the
  `PageHeader`/`ConsoleDescriptor` directly and `bootlogd` reads the console
  descriptor (no separate loader module and no side file).
- `ports/osdrv/patches`: the CMDQU `IP_SYSTEM` doorbell kernel patch.
- `tools`: `rtos-mode` (boot the C906L remoteproc) and `cop` (the generic page
  tool: status/dump/log/watch/send/wait-console/wipe-shared, driven only by the
  page's own header and console descriptor).
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

The page layout itself is not re-derived here: the firmware's C++ headers own it,
the page header and console descriptor carry it at runtime, and `cop`/`bootlogd`
read it directly. `scripts/ci/validate.py` fails the build unless the DTS
`rtos-pages` reg, `configs/maixcam/memmap.py` and `firmware-view.json` agree. The
one fact the two sides must agree on -- the page base/size -- is declared once on
each side and checked, never copied again.

## Build ownership

Version sources are deliberately separate:

- `toolchain.env`: Docker base snapshot and host cross-toolchain commit.
- `versions.env`: Linux, boot, multimedia, firmware, and package source pins.
- `components/rtos-fw/firmware/sha256sums.txt`: C906L firmware artifact pins.

The Docker image contains tools only. The active checkout mounts as
`/workspace`, `scripts` as `/builder`, and `configs` as `/configs`. Therefore a
branch never needs a branch-specific Docker tag and CI never runs stale scripts
baked into an image.

## CI responsibilities

The local runner provides the fast persistent development loop. CI always uses
an empty container build tree:

```text
ci.yml       host tests -> C906L -> Linux tools -> affected clean board builds
images.yml   matrix clean build -> format-aware packaging -> optional release
toolchain.yml immutable toolchain image, only on toolchain input changes
```

`configs/build-matrix.json` is the only list of supported board/storage/output
combinations. Both workflows consume JSON emitted by `plan.py`.

The matrix is the product baseline, not the list of boards the tree can build:
it holds the one board that ships (`maixcam-sc035hgs`), so `ci.yml` builds a
board only when a change reaches it and `images.yml` builds just that board for
a release.  The other configurations under `configs/` still build locally
(`make image BOARD=duos`); they are simply not what CI spends runners on.  To
put one back into CI, add its entry to the matrix — never a board-name
conditional to a workflow.
