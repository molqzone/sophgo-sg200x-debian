#!/usr/bin/env bash
set -euo pipefail

BOARD=${BOARD:-licheervnano}
SDK_BRANCH=${SDK_BRANCH:-master}
SDK_REPO=${SDK_REPO:-https://github.com/milkv-duo/duo-buildroot-sdk-v2.git}
OUT_DIR=${OUT_DIR:-$(pwd)/rtos_out}
BUILD_HOST_TOOLS=${BUILD_HOST_TOOLS:-$HOME/runs/duo-buildroot-sdk-v2/host-tools}
SDK_DIR=${SDK_DIR:-$HOME/runs/duo-buildroot-sdk-v2}
PATCH_ROOT=${PATCH_ROOT:-scripts/addons/rtos-firmware/patches}

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

if [ ! -d "$PATCH_ROOT" ] && [ -d rtos_sdk_patch ]; then
  PATCH_ROOT=rtos_sdk_patch
fi

if [ ! -d "$PATCH_ROOT" ]; then
  echo "RTOS patch root not found: $PATCH_ROOT" >&2
  exit 1
fi

if [ ! -d "$SDK_DIR/.git" ]; then
  git clone --depth 1 --branch "$SDK_BRANCH" "$SDK_REPO" "$SDK_DIR"
fi

if [ ! -d "$BUILD_HOST_TOOLS" ]; then
  git clone --depth 1 https://github.com/milkv-duo/host-tools.git "$BUILD_HOST_TOOLS"
fi

cp "configs/$BOARD/memmap.py" "$SDK_DIR/$SDK_MEMMAP"

stage_patch_file() {
  local rel="$1"
  local src="$PATCH_ROOT/$rel"
  local dst="$SDK_DIR/$rel"

  if [ ! -f "$src" ]; then
    echo "Missing patch file: $src" >&2
    exit 1
  fi

  mkdir -p "$(dirname "$dst")"
  cp -a "$src" "$dst"
}

for rel in \
  cvi_mpi/include/rtos_cmdqu.h \
  freertos/cvitek/driver/gpio/include/gpio.h \
  freertos/cvitek/driver/gpio/src/gpio.c \
  freertos/cvitek/driver/rtos_cmdqu.h \
  freertos/cvitek/driver/rtos_cmdqu/include/rtos_cmdqu.h \
  freertos/cvitek/task/CMakeLists.txt \
  freertos/cvitek/task/comm/CMakeLists.txt \
  freertos/cvitek/task/comm/src/riscv64/comm_main.c
do
  stage_patch_file "$rel"
done

rm -rf "$SDK_DIR/freertos/cvitek/build" "$SDK_DIR/freertos/cvitek/install"

export PATH="$BUILD_HOST_TOOLS/gcc/riscv64-elf-x86_64/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"

pushd "$SDK_DIR" >/dev/null
export TOP="$SDK_DIR"
set +u
source build/envsetup_milkv.sh "$SDK_BOARD" >/dev/null
build_rtos
set -u
popd >/dev/null

cp "$SDK_DIR/freertos/cvitek/install/bin/cvirtos.elf" "$OUT_DIR/${BOARD}_c906-mcu.elf"
cp "$SDK_DIR/freertos/cvitek/install/bin/cvirtos.bin" "$OUT_DIR/${BOARD}_c906-mcu.bin"

sha256sum "$OUT_DIR/${BOARD}_c906-mcu.elf" "$OUT_DIR/${BOARD}_c906-mcu.bin" > "$OUT_DIR/SHA256SUMS.txt"

echo "RTOS firmware built:"
echo "  $OUT_DIR/${BOARD}_c906-mcu.elf"
echo "  $OUT_DIR/${BOARD}_c906-mcu.bin"
