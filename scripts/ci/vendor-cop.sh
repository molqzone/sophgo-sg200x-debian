#!/usr/bin/env bash
# vendor-cop.sh - take `cop` from its own repository at a pinned commit.
#
# The image ships a copy of the tool, not a submodule and not a network fetch at build
# time: the build must stay reproducible from the tree.  What keeps the copy honest is
# the pin next to it (repo + commit + sha256), which `validate.py` enforces -- so the
# only supported way to change the tool here is this script, and it always moves the pin
# together with the file.
#
# Usage: scripts/ci/vendor-cop.sh [REF]     REF defaults to main (a branch or a commit)
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
ref=${1:-main}
repo=molqzone/cop
tool="$repo_root/components/guidance-fw/tools/cop"
pin="$repo_root/components/guidance-fw/tools/cop.upstream"

command -v gh >/dev/null 2>&1 || { echo "vendor-cop.sh needs the gh CLI" >&2; exit 1; }

commit=$(gh api "repos/$repo/commits/$ref" --jq .sha)
gh api "repos/$repo/contents/cop?ref=$commit" --jq .content | base64 -d > "$tool"
chmod 0755 "$tool"
digest=$(sha256sum "$tool" | cut -d' ' -f1)
printf 'repo=https://github.com/%s\ncommit=%s\nsha256=%s\n' "$repo" "$commit" "$digest" > "$pin"
echo "vendored cop from $repo@${commit:0:12} ($digest)"
python3 "$script_dir/validate.py" --board maixcam --storage sd \
	--output "$repo_root/output" --allow-missing-image
