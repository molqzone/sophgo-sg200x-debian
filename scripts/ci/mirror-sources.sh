#!/usr/bin/env bash
# mirror-sources.sh - fill the offline source cache with exactly the pinned revisions.
#
# `OFFLINE=1` clones from `$(OFFLINE_CACHE)/git/<repo>.git` instead of the network, but
# nothing ever populates that directory: when a checkout is deleted (`make clean`, or a
# failed re-clone) an offline build cannot get it back, and cache.py then reports every
# layer as "build output incomplete".  这个脚本就是补那一半的：把 versions.env 里 pin 的
# 每个仓库做成 bare mirror，并**逐个确认 pin 的提交真的在里面**。
#
# 本地 `.cache/source-cache` 里已有的镜像优先拿来克隆（省带宽），但 origin 一律写回
# versions.env 里的上游 URL，之后 `git fetch` 才有意义。
#
# Usage:
#   scripts/ci/mirror-sources.sh                 # everything in versions.env
#   scripts/ci/mirror-sources.sh --only fsbl opensbi
#   scripts/ci/mirror-sources.sh --list          # just say what is there / what is missing
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
versions=${VERSION_FILE:-$repo_root/versions.env}
mirror_root=${OFFLINE_CACHE:-$repo_root/offline-cache}/git
local_cache=${SOURCE_CACHE:-$repo_root/.cache/source-cache}

only=()
list_only=0
while [[ $# -gt 0 ]]; do
	case "$1" in
		--only) shift; while [[ $# -gt 0 && $1 != --* ]]; do only+=("$1"); shift; done ;;
		--list) list_only=1; shift ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

[ -f "$versions" ] || { echo "missing $versions" >&2; exit 1; }
mkdir -p "$mirror_root"

# versions.env is KEY=value; every component that is cloned from a repo has KEY_REPO and
# optionally KEY_COMMIT (branch-only components have no commit to verify).
declare -A repos commits
while IFS='=' read -r key value; do
	case "$key" in
		# versions.env is allowed to have CRLF endings; a stray \r turns every URL into
		# "Malformed input to a URL function", so strip it here rather than debugging it later.
		*_REPO) repos[${key%_REPO}]=${value%\"}; repos[${key%_REPO}]=${repos[${key%_REPO}]%$'\r'} ;;
		*_COMMIT) commits[${key%_COMMIT}]=${value%\"}; commits[${key%_COMMIT}]=${commits[${key%_COMMIT}]%$'\r'} ;;
	esac
done < <(grep -E '^[A-Z0-9_]+_(REPO|COMMIT)=' "$versions" | sed 's/^\([A-Z0-9_]*\)=\?\(.*\)$/\1=\2/')

select_component() {
	[[ ${#only[@]} -eq 0 ]] && return 0
	local component=$1
	for want in "${only[@]}"; do [[ ${want,,} == ${component,,} ]] && return 0; done
	return 1
}

missing=()
printf '%-18s %-34s %-9s %s\n' COMPONENT MIRROR COMMIT NOTE
for component in $(printf '%s\n' "${!repos[@]}" | sort); do
	url=${repos[$component]}
	commit=${commits[$component]:-}
	name=$(basename "$url")
	target="$mirror_root/$name"
	select_component "$component" || continue

	if [[ $list_only == 1 ]]; then
		if [[ ! -d "$target" ]]; then
			printf '%-18s %-34s %-9s %s\n' "$component" "$name" "-" "not mirrored"
			missing+=("$component")
			continue
		fi
		if [[ -n $commit ]] && ! git -C "$target" cat-file -e "${commit}^{commit}" 2>/dev/null; then
			printf '%-18s %-34s %-9s %s\n' "$component" "$name" "${commit:0:8}" "commit absent"
			missing+=("$component")
			continue
		fi
		printf '%-18s %-34s %-9s %s\n' "$component" "$name" "${commit:0:8}" "ok"
		continue
	fi

	note=fetched
	if [[ ! -d "$target" ]]; then
		if [[ -d "$local_cache/$name" ]]; then
			git clone --mirror --quiet "$local_cache/$name" "$target"
			note="from .cache/source-cache"
		else
			git clone --mirror --quiet "$url" "$target"
			note="from upstream"
		fi
	fi
	git -C "$target" remote set-url origin "$url"
	git -C "$target" fetch --quiet --prune origin || note="$note (fetch failed)"

	if [[ -n $commit ]] && ! git -C "$target" cat-file -e "${commit}^{commit}" 2>/dev/null; then
		git -C "$target" fetch --quiet origin '+refs/heads/*:refs/heads/*' '+refs/tags/*:refs/tags/*' || true
	fi
	if [[ -n $commit ]] && ! git -C "$target" cat-file -e "${commit}^{commit}" 2>/dev/null; then
		printf '%-18s %-34s %-9s %s\n' "$component" "$name" "${commit:0:8}" "MISSING pinned commit"
		missing+=("$component")
		continue
	fi
	printf '%-18s %-34s %-9s %s\n' "$component" "$name" "${commit:0:8}" "$note"
done

if [[ ${#missing[@]} -gt 0 ]]; then
	echo
	echo "not usable offline: ${missing[*]}" >&2
	echo "these need network (or a copy of the sources) once; after that OFFLINE=1 can rebuild from $mirror_root" >&2
	exit 1
fi
echo
echo "offline source cache is complete: $mirror_root"
