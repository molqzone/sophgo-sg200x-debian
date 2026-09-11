#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ci_dir=$(cd "$script_dir/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf -- "$tmp_dir"' EXIT

mkdir -p "$tmp_dir/img-input" "$tmp_dir/img-package"

img_artifact=$(python3 "$ci_dir/plan.py" artifact --board maixcam-sc035hgs --storage sd)

printf 'image-fixture\n' >"$tmp_dir/img-input/$img_artifact"
printf 'deb-fixture\n' >"$tmp_dir/img-input/test.deb"

"$ci_dir/package-board.sh" maixcam-sc035hgs sd \
	"$tmp_dir/img-input" "$tmp_dir/img-package"

test -s "$tmp_dir/img-package/$img_artifact.lz4"
test -s "$tmp_dir/img-package/maixcam-sc035hgs-sd_debs.tar.gz"
grep -Fq "$img_artifact.lz4" "$tmp_dir/img-package/SHA256SUMS.txt"

printf 'package-test=PASS img=%s\n' "$img_artifact"
