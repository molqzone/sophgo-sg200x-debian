# Local incremental builds

## Prerequisites

Docker Desktop must be running with enough disk space for the vendor SDK and
kernel trees. On Windows, set the proxy in the invoking PowerShell session when
required:

```powershell
$env:HTTP_PROXY="http://127.0.0.1:7897"
$env:HTTPS_PROXY="http://127.0.0.1:7897"
```

The PowerShell launcher maps localhost to `host.docker.internal`. General
HTTPS traffic uses the configured proxy for GitHub and the SOPHGO repository.
APT downloads are direct by default and use Debian's official HTTPS CDN. Set
`APT_HTTP_PROXY` or `APT_HTTPS_PROXY` only when APT also needs a proxy.

The Debian mirror is configurable without changing the image's installed APT
sources. For example, a mainland China development host can use:

```powershell
$env:DEBIAN_MIRROR="https://mirrors.ustc.edu.cn/debian"
```

CI keeps the default `https://deb.debian.org/debian`, so local routing choices
do not change release behavior.

The first command builds the pinned toolchain image and creates three named
volumes:

```text
sg2002-sdk     pinned vendor SDK mirror
sg2002-build   per-board source and build trees
sg2002-ccache  compiler object cache
```

## Development levels

Use the smallest target matching the change:

```text
application or protocol       make test; make firmware
Linux IPC library/OSdrv       make modules
kernel, DTS, or memory map    make image BOARD=maixcam
release candidate             make verify, then full GitHub image matrix
```

PowerShell uses `scripts/ci/local-build.ps1` with the corresponding target.
Outputs go to `output/` by default.

## Cache invalidation

`scripts/ci/cache.py` hashes maintained inputs and deletes complete affected
source trees where replaying patches would be unsafe. Invalidations propagate:

```text
firmware   -> FSBL -> rootfs/image
kernel     -> OSdrv -> middleware -> rootfs/image
OSdrv      -> middleware -> rootfs/image
middleware -> rootfs/image
boot       -> rootfs/image
```

An unchanged firmware source hash reuses the SDK worktree and copies the
previous ELF/BIN directly. This makes a hot build measurable and prevents stamp
files from hiding source changes.

## Measurements

Cold and hot runs should be recorded with the same command and board. The
project audit records wall time, cache result, ELF section layout, and SHA-256
for both outputs. A hot run is valid only when it reports cache hits and the
ELF/BIN hashes match the preceding cold run.
