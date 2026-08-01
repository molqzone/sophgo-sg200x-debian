#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
owner=${1:-}
hash=$(
	for file in "$repo_root/scripts/Dockerfile" "$repo_root/toolchain.env"; do
		sha256sum "$file" | cut -d ' ' -f1
	done | sha256sum | cut -c1-12
)

if [ -n "$owner" ]; then
	printf 'ghcr.io/%s/sophgo-sg200x-debian:toolchain-%s\n' "${owner,,}" "$hash"
else
	printf 'sg2002-toolchain:%s\n' "$hash"
fi
