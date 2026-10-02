#!/usr/bin/env bash
# Stage everything an OFFLINE build needs on a GFW-blocked host.
#
# Every source the kernel/osdrv/middleware builds use lives on github.com,
# whose TLS transfers the GFW resets mid-stream for curl and git.  gh (Go TLS)
# passes, so the pinned trees are fetched as tarballs through `gh api` and
# turned into local git mirrors; the SDK's OFFLINE=1 mode then clones from
# offline-cache/git instead of the network.  Two traps this script exists to
# handle:
#
#   1. Debian/Debian-based .gitignore rules in these repos exclude include/,
#      lib/ and bin/, so the mirrors must be built with `git add -f`.  A plain
#      `git add -A` silently drops modules/isp/include/* and the middleware
#      build dies with "cp: cannot stat 'modules/isp/include/*'".
#   2. git tarballs carry no git metadata, so `git checkout <pinned-sha>` in
#      the SDK Makefile would fail.  Each mirror therefore carries a tag with
#      the pinned commit's short form, which git resolves as a ref.
#
# The builder image normally git-fetches milkv-duo/host-tools inside the image
# build, which is exactly what the GFW breaks; here the pinned tree is fetched
# the same way and COPY'd in, then tagged with the name toolchain-ref.sh
# derives so local-build.sh picks it up unchanged.
#
# Prerequisites: gh (authenticated), docker, curl, tar.
# Usage:
#   ./scripts/stage_offline_sources.sh                 # mirrors + builder image
#   ./scripts/stage_offline_sources.sh --skip-image    # mirrors only
#   OFFLINE=1 SDK_CACHE_NO_VERIFY=1 make middleware BOARD=maixcam-sc035hgs
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE="${GUIDANCE_DEB_CACHE:-${HOME}/.cache/guidance-board/tarballs}"
OFFLINE_CACHE="${OFFLINE_CACHE:-${ROOT}/offline-cache}"
SKIP_IMAGE=0
[ "${1:-}" = "--skip-image" ] && SKIP_IMAGE=1

# Pin sources: the SDK's own versions.env / toolchain.env.
# The SDK files carry CRLF line endings; strip them when sourcing.
# shellcheck disable=SC1090
source <(tr -d '\r' < "${ROOT}/versions.env")
# shellcheck disable=SC1090
source <(tr -d '\r' < "${ROOT}/toolchain.env")

log() { echo "[$(date -Is)] $*"; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 1; }; }
need gh; need curl; need tar

gh_tarball() {  # repo ref outfile
  local repo=$1 ref=$2 out=$3 attempt
  local -a proxy_env=()
  # An empty HTTPS_PROXY makes gh hang; only set it when a proxy is configured.
  [ -n "${GH_PROXY:-}" ] && proxy_env=(HTTPS_PROXY="${GH_PROXY}")
  for attempt in $(seq 1 30); do
    # Probe first: a dead proxy node turns the transfer into an hour-long hang.
    if ! timeout 20 env "${proxy_env[@]}" gh api rate_limit --jq .rate.remaining >/dev/null 2>&1; then
      log "proxy unreachable (attempt ${attempt}); check GH_PROXY / the proxy node"
      sleep 30
      continue
    fi
    if timeout 3600 env "${proxy_env[@]}" gh api "repos/${repo}/tarball/${ref}" > "${out}.part" 2>/dev/null; then
      if tar -tzf "${out}.part" >/dev/null 2>&1; then
        mv "${out}.part" "${out}"
        log "fetched ${repo}@${ref} ($(stat -c%s "${out}") bytes)"
        return 0
      fi
    fi
    rm -f "${out}.part"
    log "retry ${attempt} for ${repo}@${ref}"
    sleep 15
  done
  echo "gave up on ${repo}@${ref}" >&2
  return 1
}

mirror_from_tarball() {  # tarball dest branch short-sha
  local tarball=$1 dest=$2 branch=$3 sha=$4 tmp
  tmp="$(mktemp -d)"
  tar -xzf "${tarball}" -C "${tmp}"
  rm -rf "${dest}"
  cp -a "${tmp}"/*/ "${dest}/"
  rm -rf "${tmp}"
  git -C "${dest}" init -q
  git -C "${dest}" config user.email "offline-mirror@local"
  git -C "${dest}" config user.name "offline mirror"
  # -f is required: the repos' .gitignore rules exclude include/, lib/, bin/.
  git -C "${dest}" add -Af
  git -C "${dest}" commit -q -m "pinned tree ${sha}"
  git -C "${dest}" branch -M "${branch}"
  git -C "${dest}" tag "${sha}"
  log "mirror $(basename "${dest}"): $(git -C "${dest}" ls-files | wc -l) files, tag ${sha}"
}

mkdir -p "${CACHE}" "${OFFLINE_CACHE}/git"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# --- kernel / osdrv / middleware / sensor support ---------------------------
gh_tarball "sophgo/linux_5.10"        "${LINUX_COMMIT}"          "${CACHE}/linux_5.10.tar.gz"
gh_tarball "sophgo/osdrv"             "${OSDRV_COMMIT}"          "${CACHE}/osdrv.tar.gz"
gh_tarball "sophgo/cvi_mpi"           "${MIDDLEWARE_COMMIT}"     "${CACHE}/cvi_mpi.tar.gz"
gh_tarball "sophgo/SensorSupportList" "${SENSOR_SUPPORT_COMMIT}" "${CACHE}/SensorSupportList.tar.gz"

mirror_from_tarball "${CACHE}/linux_5.10.tar.gz"        "${OFFLINE_CACHE}/git/linux_5.10.git"        "${LINUX_BRANCH}"              "${LINUX_COMMIT}"
mirror_from_tarball "${CACHE}/osdrv.tar.gz"             "${OFFLINE_CACHE}/git/osdrv.git"             "${OSDRV_BRANCH}"              "${OSDRV_COMMIT}"
mirror_from_tarball "${CACHE}/cvi_mpi.tar.gz"           "${OFFLINE_CACHE}/git/cvi_mpi.git"           "${MIDDLEWARE_BRANCH}"         "${MIDDLEWARE_COMMIT}"
mirror_from_tarball "${CACHE}/SensorSupportList.tar.gz" "${OFFLINE_CACHE}/git/SensorSupportList.git" "${SENSOR_SUPPORT_BRANCH}"     "${SENSOR_SUPPORT_COMMIT}"

# --- media server (ireader) with its submodules ------------------------------
gh_tarball "scpcom/ireader" "${MEDIA_SERVER_COMMIT}" "${CACHE}/ireader.tar.gz"
tar -xzf "${CACHE}/ireader.tar.gz" -C "${WORK}"
IREADER_DIR="$(echo "${WORK}"/*ireader-*/ | head -1)"

# Submodule content is not in the tarball; resolve each gitlink from the tree
# API and fetch the submodule repositories the .gitmodules names.
python3 - "${MEDIA_SERVER_COMMIT}" "${WORK}" <<'PY' > "${WORK}/submodules.tsv"
import configparser, glob, json, subprocess, sys

sha, work = sys.argv[1:3]
tree = json.loads(subprocess.run(
    ["gh", "api", f"repos/scpcom/ireader/git/trees/{sha}"],
    capture_output=True, check=True).stdout)["tree"]
links = {e["path"]: e["sha"] for e in tree if e["type"] == "commit"}
cp = configparser.ConfigParser()
cp.read(glob.glob(f"{work}/*ireader-*/.gitmodules")[0])
for section in cp.sections():
    path = cp[section]["path"]
    repo = cp[section]["url"].rstrip("/").rsplit("/", 1)[-1].removesuffix(".git")
    print(f"{repo}\t{links[path]}\t{path}")
PY

while IFS=$'\t' read -r repo sha path; do
  gh_tarball "scpcom/${repo}" "${sha}" "${CACHE}/${repo}.tar.gz"
  rm -rf "${IREADER_DIR:?}/${path}"
  mkdir -p "${IREADER_DIR}/${path}"
  tar -xzf "${CACHE}/${repo}.tar.gz" -C "${IREADER_DIR}/${path}" --strip-components=1
done < "${WORK}/submodules.tsv"

# A cloned --recursive would try to fetch the submodule URLs from github.
rm -f "${IREADER_DIR}/.gitmodules"

ireader_tarball="${CACHE}/ireader-full.tar.gz"
tar -czf "${ireader_tarball}" -C "${WORK}" "$(basename "${IREADER_DIR}")"
mirror_from_tarball "${ireader_tarball}" "${OFFLINE_CACHE}/git/ireader.git" "${MEDIA_SERVER_BRANCH}" "${MEDIA_SERVER_COMMIT}"

# --- builder image (host-tools baked in) --------------------------------------
IMAGE_NAME="$(bash "${ROOT}/scripts/ci/toolchain-ref.sh")"
if [ "${SKIP_IMAGE}" = 1 ]; then
  log "skipping builder image (--skip-image)"
elif docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
  log "builder image ${IMAGE_NAME} already present"
else
  log "fetching host-tools@${HOST_TOOLS_COMMIT} (large, be patient)"
  gh_tarball "milkv-duo/host-tools" "${HOST_TOOLS_COMMIT}" "${CACHE}/host-tools.tar.gz"
  CTX="${WORK}/builder-ctx"
  mkdir -p "${CTX}"
  tar -xzf "${CACHE}/host-tools.tar.gz" -C "${CTX}"
  mv "${CTX}"/host-tools-* "${CTX}/host-tools"
  rm -rf "${CTX}/host-tools/.git"
  rm -rf "${CTX}/host-tools/gcc/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-elf" \
         "${CTX}/host-tools/gcc/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu" \
         "${CTX}/host-tools/gcc/gcc-linaro-6.3.1-2017.05-x86_64_arm-linux-gnueabihf"
  # Same package set as scripts/Dockerfile; only the host-tools step differs
  # (COPY from the fetched tree instead of a git fetch the GFW would reset).
  cat > "${CTX}/Dockerfile" <<DOCKERFILE
FROM ${BUILDER_BASE_IMAGE} AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG DEBIAN_SNAPSHOT

RUN printf 'Acquire::Check-Valid-Until "false";\n' > /etc/apt/apt.conf.d/99snapshot \\
    && rm -f /etc/apt/sources.list.d/* \\
    && printf 'deb http://snapshot.debian.org/archive/debian/%s sid main\n' "${DEBIAN_SNAPSHOT}" > /etc/apt/sources.list

RUN apt-get update \\
    && apt-get install -y eatmydata \\
    && eatmydata apt-get install -y --no-install-recommends \\
        arch-test autoconf automake autotools-dev bc binfmt-support bison \\
        build-essential ca-certificates ccache cmake cpio curl \\
        device-tree-compiler dh-exec dh-make dosfstools e2fsprogs fakeroot \\
        fdisk flex gawk gcc-riscv64-unknown-elf gdisk genimage git gnupg \\
        gperf joe jq kernel-wedge kmod kpartx libelf-dev libexpat-dev \\
        libgmp-dev libmpc-dev libmpfr-dev libssl-dev libtool linux-image-amd64 \\
        lz4 mc mmdebstrap mtools ninja-build openssl parted patchutils \\
        pkg-config python-is-python3 python3 python3-dev python3-jinja2 \\
        python3-setuptools qemu-user-static quilt rsync sbuild \\
        sbuild-debian-developer-setup shellcheck sudo swig systemd-container \\
        texinfo wget xxd zip zlib1g-dev \\
    && rm -rf /var/lib/apt/lists/*

COPY host-tools /host-tools
RUN printf '%s\n' "${HOST_TOOLS_COMMIT}" > /host-tools/.pinned-commit

FROM builder AS build_image
WORKDIR /workspace
CMD ["/bin/bash"]
DOCKERFILE
  log "building ${IMAGE_NAME}"
  docker build -t "${IMAGE_NAME}" "${CTX}" >&2
fi

cat <<DONE
offline sources staged in ${OFFLINE_CACHE}/git:
$(ls "${OFFLINE_CACHE}/git")

Build the board middleware without network access:
  cd ${ROOT}
  OFFLINE=1 SDK_CACHE_NO_VERIFY=1 make middleware BOARD=maixcam-sc035hgs
DONE
