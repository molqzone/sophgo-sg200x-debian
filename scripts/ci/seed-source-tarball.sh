#!/usr/bin/env bash
# seed-source-tarball.sh - rebuild an offline source mirror from a GitHub tarball.
#
# 当 `git clone` 走不通（代理只对 GitHub API 放行、或 TLS 被中断）而 `gh api` 还能用时，
# 这是唯一能把 pin 的内容取回来的办法：GitHub 的 tarball 端点给出的是**该提交的树**，内容
# 与 `git checkout <commit>` 完全一致，只是本地要重新做一个提交，所以提交号不同——
# 因此 `versions.env` 的 pin 需要在这一版构建里用命令行覆盖，脚本会把映射记录到
# offline-cache/SEEDED-SOURCES.txt。
#
# When `git clone` is impossible but the GitHub API works, this restores the pinned tree
# from a tarball.  Content is identical to the pinned commit; the local commit id is not,
# so the build needs _COMMIT overrides, recorded in offline-cache/SEEDED-SOURCES.txt.
#
# Usage: scripts/ci/seed-source-tarball.sh FSBL [OPENSBI ...]
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
versions=${VERSION_FILE:-$repo_root/versions.env}
mirror_root=${OFFLINE_CACHE:-$repo_root/offline-cache}
attempts=${ATTEMPTS:-6}

[[ $# -gt 0 ]] || { echo "usage: seed-source-tarball.sh COMPONENT [...], COMPONENT is e.g. FSBL" >&2; exit 2; }

for component in "$@"; do
	url=$(grep -E "^${component}_REPO=" "$versions" | tr -d '\r' | cut -d= -f2)
	branch=$(grep -E "^${component}_BRANCH=" "$versions" | tr -d '\r' | cut -d= -f2 || true)
	commit=$(grep -E "^${component}_COMMIT=" "$versions" | tr -d '\r' | cut -d= -f2)
	[[ -n $url && -n $commit ]] || { echo "$component: no _REPO/_COMMIT in $versions" >&2; exit 1; }
	slug=${url#https://github.com/}; slug=${slug%.git}
	name=$(basename "$url")
	branch=${branch:-main}
	work=$(mktemp -d)
	trap 'rm -rf "$work"' EXIT

	echo "== $component: $slug@$commit (branch $branch)"
	ok=0
	for attempt in $(seq 1 "$attempts"); do
		if gh api "repos/$slug/tarball/$commit" > "$work/src.tgz" 2>"$work/err"; then
			ok=1
			break
		fi
		echo "   attempt $attempt failed: $(tail -1 "$work/err")"
		sleep $((attempt * 2))
	done
	[[ $ok == 1 ]] || { echo "$component: giving up after $attempts attempts" >&2; exit 1; }

	mkdir -p "$work/src"
	tar xzf "$work/src.tgz" -C "$work/src" --strip-components=1
	(
		cd "$work/src"
		git init -q -b "$branch"
		git add -A
		git -c user.email=seed@local -c user.name=seed commit -qm \
			"content of $slug@$commit (seeded from a GitHub tarball)"
	)
	local_sha=$(git -C "$work/src" rev-parse HEAD)
	mkdir -p "$mirror_root/git"
	rm -rf "$mirror_root/git/$name"
	git clone --quiet --bare "$work/src" "$mirror_root/git/$name"
	printf '%s|%s|%s|%s|%s\n' "$component" "$slug" "$commit" "$local_sha" "$branch" \
		>> "$mirror_root/SEEDED-SOURCES.txt"
	echo "   local commit $local_sha -> $mirror_root/git/$name (override ${component}_COMMIT=$local_sha)"
done
