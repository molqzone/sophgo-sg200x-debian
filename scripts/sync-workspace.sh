#!/usr/bin/env bash
set -euo pipefail

# Top-level multi-repo developer sync script.
# Synchronizes sophgo-sg200x-debian, bsp-guidance-action, and bsp-guidance-vision,
# updates all submodules, runs `xrobot setup --frozen`, and verifies shared-page-xr
# protocol alignment between action and vision.

IMAGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKSPACE_DIR="${SG200X_WORKSPACE_DIR:-$(dirname "${IMAGE_DIR}")}"

ACTION_REPO="${GUIDANCE_ACTION_URL:-https://github.com/molqzone/bsp-guidance-action.git}"
VISION_REPO="${GUIDANCE_VISION_URL:-https://github.com/molqzone/bsp-guidance-vision.git}"
ACTION_DIR="${SG200X_ACTION_DIR:-${WORKSPACE_DIR}/bsp-guidance-action}"
VISION_DIR="${SG200X_VISION_DIR:-${WORKSPACE_DIR}/bsp-guidance-vision}"

sync_repo() {
  local dir="$1" url="$2"
  if [ ! -d "${dir}/.git" ]; then
    echo "Cloning ${url} -> ${dir}"
    git clone --recursive "${url}" "${dir}"
  else
    echo "Updating submodules in ${dir}"
    git -C "${dir}" submodule sync --recursive
    git -C "${dir}" submodule update --init --recursive
  fi
}

sync_repo "${IMAGE_DIR}" ""
sync_repo "${ACTION_DIR}" "${ACTION_REPO}"
sync_repo "${VISION_DIR}" "${VISION_REPO}"

if command -v xrobot >/dev/null 2>&1; then
  for repo_dir in "${ACTION_DIR}" "${VISION_DIR}"; do
    if [ -f "${repo_dir}/xrobot.lock" ]; then
      echo "Running xrobot setup --frozen in ${repo_dir}"
      (cd "${repo_dir}" && xrobot setup --frozen)
    fi
  done
else
  echo "WARN: xrobot CLI not found; skipping xrobot setup --frozen" >&2
fi

if [ -d "${ACTION_DIR}/shared-page-xr" ] && [ -d "${VISION_DIR}/shared-page-xr" ]; then
  action_spxr="$(git -C "${ACTION_DIR}/shared-page-xr" rev-parse HEAD)"
  vision_spxr="$(git -C "${VISION_DIR}/shared-page-xr" rev-parse HEAD)"
  if [ "${action_spxr}" != "${vision_spxr}" ]; then
    echo "ERROR: shared-page-xr mismatch! action=${action_spxr} vision=${vision_spxr}" >&2
    exit 1
  fi
  echo "Verified shared-page-xr alignment: ${action_spxr}"
fi
