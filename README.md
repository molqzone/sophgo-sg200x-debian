# Debian images for Sophgo CV181x/SG200x boards

This repository builds Debian sid images for Milk-V Duo256/DuoS, Sipeed
LicheeRV Nano, and MaixCAM. It remains a single repository so upstream board
support can be synchronized without mixing MaixCAM product policy into the
upstream configurations.

The sole build matrix is [`configs/build-matrix.json`](configs/build-matrix.json).
Inspect it with:

```sh
python3 scripts/ci/plan.py matrix
```

SD builds produce `<board>_sd.img`. DuoS eMMC produces `duos_emmc.zip`; the
packaging format comes from the matrix and is not inferred by a workflow.

## Local development

The normal development loop uses a persistent Docker SDK, build tree, and
ccache:

```sh
make test
make firmware
make modules
make image BOARD=maixcam
make verify
```

On Windows, where GNU Make is not normally installed, use the equivalent
PowerShell entry point:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/ci/local-build.ps1 -Target test
powershell -ExecutionPolicy Bypass -File scripts/ci/local-build.ps1 -Target firmware
powershell -ExecutionPolicy Bypass -File scripts/ci/local-build.ps1 -Target image -Board maixcam
powershell -ExecutionPolicy Bypass -File scripts/ci/local-build.ps1 -Target verify
```

The runner creates `sg2002-sdk`, `sg2002-build`, and `sg2002-ccache` Docker
volumes. Source hashes invalidate only the affected layers and their required
downstream consumers. See [`docs/local-build.md`](docs/local-build.md).

## Repository layers

- `components/sg2002-ipc`: standalone C906L firmware, Linux library/tools,
  host tests, and vendor SDK/OSdrv ports.
- `configs/common`: settings and patches shared by every board.
- `configs/<upstream-board>`: the original board configuration.
- `configs/maixcam`: an exact board layer derived from `licheervnano`.
- `scripts/addons/sg2002-ipc`: a thin image integration adapter.
- `scripts/ci`: commands shared by local builds and GitHub Actions.

Board files resolve in exact-board, base-board, then common precedence. Patches
apply common, base-board, then exact-board; an exact `*.patch.skip` file masks
an inherited patch with the same name. Details are in
[`docs/architecture.md`](docs/architecture.md).

## CI and releases

`ci.yml` runs host tests, builds the standalone C906L component and Linux tools,
and builds only affected matrix entries. `images.yml` performs the clean full
matrix for manual runs, schedules, and tags. `toolchain.yml` publishes the
immutable toolchain image only when its Dockerfile or pin file changes.

All workflow logic delegates to `scripts/ci`; the same commands can be run
locally. The A53 workflow is intentionally unsupported for SG2002.

## Flashing

Write an uncompressed SD image from Linux with:

```sh
sudo dd if=maixcam_sd.img of=/dev/sdX bs=4M status=progress conv=fsync
```

Use the correct target device; this overwrites it. Windows users can use a raw
image writer such as balenaEtcher. Flash `duos_emmc.zip` with the Milk-V vendor
eMMC update flow.

Default local logins are `root/rv` and `debian/rv`; root SSH login is disabled.

## Upstream maintenance

Keep upstream board behavior byte-for-byte where possible and put product
differences in `configs/maixcam` or a `maixcam-*` addon. The synchronization and
verification procedure is documented in
[`docs/upstream-sync.md`](docs/upstream-sync.md).
