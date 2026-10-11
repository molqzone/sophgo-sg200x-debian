#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
exec bash "$repo_root/scripts/ci/local-build.sh" image \
	--board "${BOARD:-maixcam}" \
	--storage "${STORAGE_TYPE:-sd}" \
	--output "${OUTPUT_DIR:-output}"
