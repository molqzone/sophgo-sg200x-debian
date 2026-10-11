#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
	echo "Usage: build-board.sh <board> <storage> <output-dir>" >&2
	exit 2
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
board=$1
storage=$2
output_dir=$(mkdir -p "$3" && cd "$3" && pwd)
image=${BUILDER_IMAGE:?BUILDER_IMAGE is required}

python3 "$script_dir/plan.py" artifact --board "$board" --storage "$storage" >/dev/null
docker pull "$image"
docker run --rm --privileged --entrypoint /bin/bash \
	-e IN_CONTAINER=1 \
	-e CCACHE_DIR=/ccache \
	-v "$repo_root:/workspace:ro" \
	-v "$repo_root/scripts:/builder:ro" \
	-v "$repo_root/configs:/configs:ro" \
	-v "$output_dir:/output" \
	-w /workspace \
	"$image" \
	/workspace/scripts/ci/local-build.sh image --inside \
	--board "$board" --storage "$storage" --output /output

python3 "$script_dir/validate.py" \
	--board "$board" --storage "$storage" --output "$output_dir"
