#!/usr/bin/env bash
set -euo pipefail

# Fetch pinned guidance firmware (action) and vision app (vision) release assets
# declared in versions.env so image builds need no cross-compilers or source trees.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
eval "$(
  tr -d '\r' < "${REPO_ROOT}/versions.env" \
    | grep -E '^GUIDANCE_[A-Z0-9_]+=' \
    | sed 's/^/export /'
)"

DO_ACTION=1
DO_VISION=1
for arg in "$@"; do
  case "$arg" in
    --action-only) DO_VISION=0 ;;
    --vision-only) DO_ACTION=0 ;;
    *)
      echo "Usage: $0 [--action-only|--vision-only]" >&2
      exit 2
      ;;
  esac
done

download_asset() {
  local repo="$1" tag="$2" asset="$3" dest="$4"
  if command -v gh >/dev/null 2>&1; then
    gh release download "${tag}" -R "${repo}" -p "${asset}" -O "${dest}" --clobber
  else
    curl -fsSL "https://github.com/${repo}/releases/download/${tag}/${asset}" -o "${dest}"
  fi
}

verify_sha256() {
  local file="$1" expected="$2"
  if [ -z "${expected}" ]; then
    echo "WARN: no SHA256 pin for ${file}; skipping checksum verification" >&2
    return 0
  fi
  local actual
  actual="$(sha256sum "${file}" | awk '{print $1}')"
  if [ "${actual}" != "${expected}" ]; then
    echo "ERROR: ${file} sha256=${actual}, expected ${expected}" >&2
    exit 1
  fi
}

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

if [ "${DO_ACTION}" = "1" ]; then
  if [ -z "${GUIDANCE_ACTION_RELEASE}" ]; then
    echo "GUIDANCE_ACTION_RELEASE is unset in versions.env; skipping action artifacts."
  else
    download_asset "${GUIDANCE_ACTION_REPO}" "${GUIDANCE_ACTION_RELEASE}" "c906-mcu.elf" "${TMP_DIR}/c906-mcu.elf"
    download_asset "${GUIDANCE_ACTION_REPO}" "${GUIDANCE_ACTION_RELEASE}" "c906-mcu.bin" "${TMP_DIR}/c906-mcu.bin"
    verify_sha256 "${TMP_DIR}/c906-mcu.elf" "${GUIDANCE_ACTION_ELF_SHA256}"
    verify_sha256 "${TMP_DIR}/c906-mcu.bin" "${GUIDANCE_ACTION_BIN_SHA256}"

    FW_DIR="${REPO_ROOT}/components/rtos-fw/firmware"
    install -m 0644 "${TMP_DIR}/c906-mcu.elf" "${FW_DIR}/c906-mcu.elf"
    install -m 0644 "${TMP_DIR}/c906-mcu.bin" "${FW_DIR}/c906-mcu.bin"
    (cd "${FW_DIR}" && sha256sum c906-mcu.elf c906-mcu.bin > sha256sums.txt)
    echo "Updated ${FW_DIR} from ${GUIDANCE_ACTION_REPO}@${GUIDANCE_ACTION_RELEASE}"
  fi
fi

if [ "${DO_VISION}" = "1" ]; then
  if [ -z "${GUIDANCE_VISION_RELEASE}" ]; then
    echo "GUIDANCE_VISION_RELEASE is unset in versions.env; skipping vision artifacts."
  else
    download_asset "${GUIDANCE_VISION_REPO}" "${GUIDANCE_VISION_RELEASE}" "guidance_vision_app" "${TMP_DIR}/guidance_vision_app"
    verify_sha256 "${TMP_DIR}/guidance_vision_app" "${GUIDANCE_VISION_APP_SHA256}"

    VISION_BIN="${REPO_ROOT}/scripts/addons/guidance-autostart/files/guidance_vision_app"
    install -m 0755 "${TMP_DIR}/guidance_vision_app" "${VISION_BIN}"
    echo "Updated ${VISION_BIN} from ${GUIDANCE_VISION_REPO}@${GUIDANCE_VISION_RELEASE}"
  fi
fi
