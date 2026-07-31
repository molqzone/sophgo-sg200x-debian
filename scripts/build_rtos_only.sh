#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

BOARD=${BOARD:-licheervnano}
ARTIFACT_BOARD=$BOARD
SDK_REPO=${SDK_REPO:-https://github.com/milkv-duo/duo-buildroot-sdk-v2.git}
SDK_COMMIT=${SDK_COMMIT:-6f8962c394dd0a05729abb089f0feb7d5cc4aa5e}
OUT_DIR=${OUT_DIR:-$(pwd)/rtos_out}
BUILD_HOST_TOOLS=${BUILD_HOST_TOOLS:-$HOME/runs/duo-buildroot-sdk-v2/host-tools}
SDK_DIR=${SDK_DIR:-$HOME/runs/duo-buildroot-sdk-v2}
ADDON_ROOT=${ADDON_ROOT:-$SCRIPT_DIR/addons/rtos-firmware}
CONFIG_ROOT=${CONFIG_ROOT:-$REPO_ROOT/configs}
CHECK_TRACE_SCRIPT=${CHECK_TRACE_SCRIPT:-$SCRIPT_DIR/check_rtos_trace_layout.py}

case "$BOARD" in
  licheervnano|duo256)
    SDK_BOARD=milkv-duo256m-musl-riscv64-sd
    SDK_MEMMAP=build/boards/cv181x/sg2002_milkv_duo256m_musl_riscv64_sd/memmap.py
    ;;
  duos)
    SDK_BOARD=milkv-duos-musl-riscv64-sd
    SDK_MEMMAP=build/boards/cv181x/sg2000_milkv_duos_musl_riscv64_sd/memmap.py
    ;;
  *)
    echo "Unsupported BOARD=$BOARD" >&2
    exit 2
    ;;
esac

mkdir -p "$OUT_DIR"

if [ ! -d "$ADDON_ROOT" ]; then
  echo "RTOS addon root not found: $ADDON_ROOT" >&2
  exit 1
fi
ADDON_ROOT=$(cd "$ADDON_ROOT" && pwd)

if [ ! -d "$SDK_DIR/.git" ]; then
  mkdir -p "$SDK_DIR"
  git -C "$SDK_DIR" init
fi

if ! git -C "$SDK_DIR" remote get-url origin >/dev/null 2>&1; then
  git -C "$SDK_DIR" remote add origin "$SDK_REPO"
fi
git -C "$SDK_DIR" remote set-url origin "$SDK_REPO"
git -C "$SDK_DIR" fetch --depth 1 origin "$SDK_COMMIT"
git -C "$SDK_DIR" checkout --force --detach FETCH_HEAD

if [ "$(git -C "$SDK_DIR" rev-parse HEAD)" != "$SDK_COMMIT" ]; then
  echo "RTOS SDK commit verification failed" >&2
  exit 1
fi

if [ ! -d "$BUILD_HOST_TOOLS" ]; then
  git clone --depth 1 https://github.com/milkv-duo/host-tools.git "$BUILD_HOST_TOOLS"
fi

cp "$CONFIG_ROOT/$BOARD/memmap.py" "$SDK_DIR/$SDK_MEMMAP"
bash "$ADDON_ROOT/prepare_sdk.sh" "$SDK_DIR"

rm -rf "$SDK_DIR/freertos/cvitek/build" "$SDK_DIR/freertos/cvitek/install"

export PATH="$BUILD_HOST_TOOLS/gcc/riscv64-elf-x86_64/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"

pushd "$SDK_DIR" >/dev/null
export TOP="$SDK_DIR"
set +u
source build/envsetup_milkv.sh "$SDK_BOARD" >/dev/null
build_rtos
set -u
popd >/dev/null

cp "$SDK_DIR/freertos/cvitek/install/bin/cvirtos.elf" "$OUT_DIR/${ARTIFACT_BOARD}_c906-mcu.elf"
cp "$SDK_DIR/freertos/cvitek/install/bin/cvirtos.bin" "$OUT_DIR/${ARTIFACT_BOARD}_c906-mcu.bin"

python3 "$CHECK_TRACE_SCRIPT" \
  --readelf "$BUILD_HOST_TOOLS/gcc/riscv64-elf-x86_64/bin/riscv64-unknown-elf-readelf" \
  "$OUT_DIR/${ARTIFACT_BOARD}_c906-mcu.elf" |
  tee "$OUT_DIR/${ARTIFACT_BOARD}_boot-trace-layout.json"

sha256sum "$OUT_DIR/${ARTIFACT_BOARD}_c906-mcu.elf" \
  "$OUT_DIR/${ARTIFACT_BOARD}_c906-mcu.bin" > "$OUT_DIR/SHA256SUMS.txt"
git -C "$SDK_DIR" rev-parse HEAD > \
  "$OUT_DIR/${ARTIFACT_BOARD}_rtos-sdk-commit.txt"
sha256sum \
	"$ADDON_ROOT/include/sg2002_rtos_protocol.h" \
	"$ADDON_ROOT/include/sg2002_rtos_shm.h" \
	"$ADDON_ROOT/include/sg2002_rtos_ring.h" \
	"$ADDON_ROOT/common/sg2002_rtos_ring.c" \
	"$ADDON_ROOT/freertos/include/sg2002_rtos_app.h" \
	"$ADDON_ROOT/freertos/include/sg2002_rtos_mailbox.h" \
	"$ADDON_ROOT/freertos/include/sg2002_rtos_platform.h" \
	"$ADDON_ROOT/freertos/include/sg2002_rtos_shm_transport.h" \
	"$ADDON_ROOT/freertos/src/sg2002_rtos_app.c" \
	"$ADDON_ROOT/freertos/src/sg2002_rtos_mailbox.c" \
	"$ADDON_ROOT/freertos/src/sg2002_rtos_shm_transport.c" \
	"$ADDON_ROOT/patches/freertos/cvitek/task/comm/CMakeLists.txt" \
	"$ADDON_ROOT/patches/freertos/cvitek/task/comm/src/riscv64/comm_main.c" > \
	"$OUT_DIR/${ARTIFACT_BOARD}_rtos-source-SHA256SUMS.txt"

echo "RTOS firmware built:"
echo "  $OUT_DIR/${ARTIFACT_BOARD}_c906-mcu.elf"
echo "  $OUT_DIR/${ARTIFACT_BOARD}_c906-mcu.bin"
