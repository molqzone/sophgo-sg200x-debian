#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck disable=SC1091
source "$repo_root/toolchain.env"
image=${BUILDER_IMAGE:-$(bash "$repo_root/scripts/ci/toolchain-ref.sh")}

docker build \
	--build-arg "BUILDER_BASE_IMAGE=$BUILDER_BASE_IMAGE" \
	--build-arg "DEBIAN_SNAPSHOT=$DEBIAN_SNAPSHOT" \
	--build-arg "HOST_TOOLS_REPO=$HOST_TOOLS_REPO" \
	--build-arg "HOST_TOOLS_COMMIT=$HOST_TOOLS_COMMIT" \
	-t "$image" -f "$repo_root/scripts/Dockerfile" "$repo_root"
printf '%s\n' "$image"
