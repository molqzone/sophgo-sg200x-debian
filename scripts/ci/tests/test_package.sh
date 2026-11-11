#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ci_dir=$(cd "$script_dir/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf -- "$tmp_dir"' EXIT

# The product matrix decides what CI builds, not how an artifact is packaged: an SD entry
# is lz4-compressed and an eMMC entry is copied as-is, and both branches have to stay
# covered whatever the matrix currently contains.  So this test carries its own matrix
# with one board of each shape.  plan.board_chain() only needs the board directories to
# exist, so empty ones are enough, and PLAN_CONFIG_ROOT points every lookup here.
config_dir="$tmp_dir/config"
mkdir -p "$config_dir/imgboard" "$config_dir/zipboard"
cat >"$config_dir/build-matrix.json" <<'JSON'
{
  "include": [
    {"board": "imgboard", "storage": "sd", "format": "img"},
    {"board": "zipboard", "storage": "emmc", "format": "zip"}
  ]
}
JSON

mkdir -p "$tmp_dir/img-input" "$tmp_dir/img-package"
mkdir -p "$tmp_dir/zip-input" "$tmp_dir/zip-package"

export PLAN_CONFIG_ROOT="$config_dir"
img_artifact=$(python3 "$ci_dir/plan.py" artifact --board imgboard --storage sd)
zip_artifact=$(python3 "$ci_dir/plan.py" artifact --board zipboard --storage emmc)

printf 'image-fixture\n' >"$tmp_dir/img-input/$img_artifact"
printf 'deb-fixture\n' >"$tmp_dir/img-input/test.deb"
printf 'zip-fixture\n' >"$tmp_dir/zip-input/$zip_artifact"

"$ci_dir/package-board.sh" imgboard sd \
	"$tmp_dir/img-input" "$tmp_dir/img-package"
"$ci_dir/package-board.sh" zipboard emmc \
	"$tmp_dir/zip-input" "$tmp_dir/zip-package"

# SD: compressed, the plain image is not shipped next to it, and the debs come along.
test -s "$tmp_dir/img-package/$img_artifact.lz4"
test ! -e "$tmp_dir/img-package/$img_artifact"
test -s "$tmp_dir/img-package/imgboard-sd_debs.tar.gz"
# eMMC: copied verbatim, never compressed, and no debs were produced for it.
test -s "$tmp_dir/zip-package/$zip_artifact"
test ! -e "$tmp_dir/zip-package/$zip_artifact.lz4"
test ! -e "$tmp_dir/zip-package/zipboard-emmc_debs.tar.gz"
grep -Fq "$img_artifact.lz4" "$tmp_dir/img-package/SHA256SUMS.txt"
grep -Fq "$zip_artifact" "$tmp_dir/zip-package/SHA256SUMS.txt"

printf 'package-test=PASS img=%s zip=%s\n' "$img_artifact" "$zip_artifact"
