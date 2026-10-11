#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
export PYTHONPYCACHEPREFIX=${PYTHONPYCACHEPREFIX:-/tmp/sg2002-python-cache}
mkdir -p "$PYTHONPYCACHEPREFIX"

python3 -m compileall -q "$script_dir"
python3 -m unittest discover -s "$script_dir/tests" -p 'test_*.py'

while IFS= read -r -d '' script; do
	bash -n "$script"
done < <(find "$repo_root" \
	-path "$repo_root/.git" -prune -o \
	-path "$repo_root/.cache" -prune -o \
	-path "$repo_root/.codex-build-lf" -prune -o \
	-path "$repo_root/offline-cache" -prune -o \
	-path "$repo_root/output" -prune -o \
	-path "$repo_root/image" -prune -o \
	-type f -name '*.sh' -print0)

python3 "$script_dir/validate.py" --allow-missing-image
bash "$script_dir/tests/test_package.sh"
