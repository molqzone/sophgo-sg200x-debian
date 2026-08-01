#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ci_dir=$(cd "$script_dir/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf -- "$tmp_dir"' EXIT

mkdir -p "$tmp_dir/img-input" "$tmp_dir/img-package"
mkdir -p "$tmp_dir/zip-input" "$tmp_dir/zip-package"

img_artifact=$(python3 "$ci_dir/plan.py" artifact --board licheervnano --storage sd)
zip_artifact=$(python3 "$ci_dir/plan.py" artifact --board duos --storage emmc)

printf 'image-fixture\n' >"$tmp_dir/img-input/$img_artifact"
printf 'deb-fixture\n' >"$tmp_dir/img-input/test.deb"
printf 'zip-fixture\n' >"$tmp_dir/zip-input/$zip_artifact"

"$ci_dir/package-board.sh" licheervnano sd \
	"$tmp_dir/img-input" "$tmp_dir/img-package"
"$ci_dir/package-board.sh" duos emmc \
	"$tmp_dir/zip-input" "$tmp_dir/zip-package"

test -s "$tmp_dir/img-package/$img_artifact.lz4"
test -s "$tmp_dir/img-package/licheervnano-sd_debs.tar.gz"
test -s "$tmp_dir/zip-package/$zip_artifact"
test ! -e "$tmp_dir/zip-package/$zip_artifact.lz4"
grep -Fq "$img_artifact.lz4" "$tmp_dir/img-package/SHA256SUMS.txt"
grep -Fq "$zip_artifact" "$tmp_dir/zip-package/SHA256SUMS.txt"

printf 'package-test=PASS img=%s zip=%s\n' "$img_artifact" "$zip_artifact"
