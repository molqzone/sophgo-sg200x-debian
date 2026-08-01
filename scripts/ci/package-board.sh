#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 4 ]; then
	echo "Usage: package-board.sh <board> <storage> <input-dir> <package-dir>" >&2
	exit 2
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
board=$1
storage=$2
input_dir=$(cd "$3" && pwd)
package_dir=$(mkdir -p "$4" && cd "$4" && pwd)
artifact=$(python3 "$script_dir/plan.py" artifact --board "$board" --storage "$storage")
source_artifact="$input_dir/$artifact"
test -s "$source_artifact"

case "$artifact" in
	*.img)
		lz4 -9 -f "$source_artifact" "$package_dir/$artifact.lz4"
		;;
	*.zip)
		cp -a "$source_artifact" "$package_dir/"
		;;
	*)
		echo "Unsupported artifact type: $artifact" >&2
		exit 2
		;;
esac

mapfile -d '' debs < <(find "$input_dir" -maxdepth 1 -type f -name '*.deb' -print0 | sort -z)
if ((${#debs[@]})); then
	tar --create --gzip --file "$package_dir/${board}-${storage}_debs.tar.gz" \
		--transform='s|.*/||' "${debs[@]}"
fi

while IFS= read -r -d '' file; do
	name=$(basename "$file")
	case "$name" in
		"$artifact"|*.deb) continue ;;
	esac
	cp -a "$file" "$package_dir/"
done < <(find "$input_dir" -maxdepth 1 -type f -print0 | sort -z)

(
	cd "$package_dir"
	find . -maxdepth 1 -type f ! -name SHA256SUMS.txt -printf '%f\0' |
		sort -z | xargs -0 -r sha256sum >SHA256SUMS.txt
)
