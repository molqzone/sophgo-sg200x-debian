#!/usr/bin/env bash
set -euo pipefail

BOARD=${BOARD:-licheervnano}
SDK_BRANCH=${SDK_BRANCH:-master}
SDK_REPO=${SDK_REPO:-https://github.com/milkv-duo/duo-buildroot-sdk-v2.git}
OUT_DIR=${OUT_DIR:-$(pwd)/rtos_out}
BUILD_HOST_TOOLS=${BUILD_HOST_TOOLS:-$HOME/runs/duo-buildroot-sdk-v2/host-tools}
SDK_DIR=${SDK_DIR:-$HOME/runs/duo-buildroot-sdk-v2}

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

if [ ! -d "$SDK_DIR/.git" ]; then
  git clone --depth 1 --branch "$SDK_BRANCH" "$SDK_REPO" "$SDK_DIR"
fi

if [ ! -d "$BUILD_HOST_TOOLS" ]; then
  git clone --depth 1 https://github.com/milkv-duo/host-tools.git "$BUILD_HOST_TOOLS"
fi

cp "configs/$BOARD/memmap.py" "$SDK_DIR/$SDK_MEMMAP"

if [ -d rtos_sdk_patch/freertos ] || [ -d rtos_sdk_patch/cvi_mpi ]; then
  rsync -a rtos_sdk_patch/ "$SDK_DIR/"
fi

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
