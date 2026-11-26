#!/usr/bin/env bash
# vendor-cop.sh - build `cop` from its own repository and pin the binary.
#
# The image ships a static RISC-V binary, not a Python file and not a submodule: the
# image build stays reproducible from the tree, and the pin (repo + commit + sha256)
# says which `molqzone/cop` revision the binary came from.  The build uses the action
# checkout's `libxr`/`shared-page-xr` so the tool and the firmware compile against the
# same transport headers; `cop` never includes a product contract.
#
# Usage: scripts/ci/vendor-cop.sh [REF]     REF defaults to main (branch or commit)
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
ref=${1:-main}
repo=molqzone/cop
tool="$repo_root/components/guidance-fw/tools/cop"
pin="$repo_root/components/guidance-fw/tools/cop.upstream"
action_dir=${SG200X_ACTION_DIR:-"$repo_root/../bsp-guidance-action"}
cop_cxx=${SG200X_COP_CXX:-riscv64-linux-gnu-g++}

case "$cop_cxx" in
  *g++) cop_cc="${cop_cxx%g++}gcc" ;;
  *clang++) cop_cc="${cop_cxx%clang++}clang" ;;
  *) cop_cc=$cop_cxx ;;
esac

for path in "$action_dir/libxr/CMakeLists.txt" "$action_dir/shared-page-xr/CMakeLists.txt"; do
  [[ -f $path ]] || {
    echo "vendor-cop.sh: missing $path; set SG200X_ACTION_DIR to the firmware checkout" >&2
    exit 1
  }
done
command -v "$cop_cxx" >/dev/null 2>&1 || {
  echo "vendor-cop.sh: $cop_cxx is not on PATH (set SG200X_COP_CXX)" >&2
  exit 1
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

git clone --quiet "https://github.com/$repo.git" "$work/cop"
git -C "$work/cop" checkout --quiet "$ref"
commit=$(git -C "$work/cop" rev-parse HEAD)

cmake -S "$work/cop" -B "$work/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$cop_cc" \
  -DCMAKE_CXX_COMPILER="$cop_cxx" \
  -DCOP_STATIC=ON \
  -DCOP_BUILD_TESTS=OFF \
  -DCOP_LIBXR_DIR="$action_dir/libxr" \
  -DCOP_SHARED_PAGE_DIR="$action_dir/shared-page-xr" >/dev/null
cmake --build "$work/build" --target cop -j"$(nproc)" >/dev/null

install -m 0755 "$work/build/cop" "$tool"
digest=$(sha256sum "$tool" | cut -d' ' -f1)
printf '# Built from %s at this commit; generic C++ CLI, no Python fallback.\nrepo=https://github.com/%s\ncommit=%s\nsha256=%s\n' \
  "$repo" "$repo" "$commit" "$digest" > "$pin"

echo "vendored cop from $repo@${commit:0:12} ($digest)"
python3 "$script_dir/validate.py" --board maixcam --storage sd \
  --output "$repo_root/output" --allow-missing-image
