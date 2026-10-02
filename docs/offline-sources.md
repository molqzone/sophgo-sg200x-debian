# Building on a GFW-blocked host

Every source the image build needs lives on github.com, whose TLS transfers are
reset mid-stream from inside China for both curl and git.  Two paths exist:

* **Container wrapper** — `./scripts/build_board.sh` builds the board CLI in an
  Ubuntu image (see `../Containerfile.board`); the middleware still needs the
  sources below.
* **Offline staging** — `./scripts/stage_offline_sources.sh` fetches the pinned
  trees once and every later build runs from local mirrors with no network.

## Offline staging

```bash
./scripts/stage_offline_sources.sh              # mirrors + builder image
# then:
OFFLINE=1 SDK_CACHE_NO_VERIFY=1 make middleware BOARD=maixcam-sc035hgs
```

Prerequisites: `gh` (authenticated), docker, curl, tar.  `gh` is what makes the
downloads possible at all: its Go TLS handshake is not fingerprinted the way
OpenSSL's is, so it survives where curl and git are reset.  Tarball downloads
follow redirects to codeload.github.com, which the GFW blackholes even for gh,
so the script routes them through a local mixed-port proxy by default
(`GH_PROXY`, default `http://127.0.0.1:7890`); set it to an empty value on a host
with direct access.

What the script stages under `offline-cache/git/`:

| mirror | pinned by | note |
|---|---|---|
| `linux_5.10.git` | `LINUX_COMMIT` | board-specific kernel |
| `osdrv.git` | `OSDRV_COMMIT` | kernel modules |
| `cvi_mpi.git` | `MIDDLEWARE_COMMIT` | CVI middleware |
| `SensorSupportList.git` | `SENSOR_SUPPORT_COMMIT` | sensor drivers |
| `ireader.git` | `MEDIA_SERVER_COMMIT` | test_mmf media server, submodules materialised |

Two non-obvious requirements the script encodes:

1. **Mirrors are built with `git add -f`.**  The repos' `.gitignore` rules
   exclude `include/`, `lib/` and `bin/`, so a plain `git add -A` silently
   drops `modules/isp/include/*` and the middleware build fails with
   `cp: cannot stat 'modules/isp/include/*'`.
2. **Each mirror carries a tag with the pinned commit's short form.**  Tarballs
   carry no git metadata, so the Makefile's `git checkout <sha>` would fail
   otherwise; git resolves the tag as a ref.

The builder image is rebuilt from a generated Dockerfile with the same apt
package set as `scripts/Dockerfile` — only the `host-tools` step differs: the
pinned tree is fetched the same way and `COPY`'d in instead of git-fetched
inside the image build.  The image is tagged with the name
`scripts/ci/toolchain-ref.sh` derives, so `local-build.sh` picks it up
unchanged.  Pass `--skip-image` when an image built earlier is still present.

## Running the build

`OFFLINE=1` makes `scripts/Makefile` clone from `offline-cache/git` instead of
github.com and makes `local-build.sh` verify the cache instead of using the
network.  `SDK_CACHE_NO_VERIFY=1` additionally lets the *first* offline build
run without a previously prepared cache: `cache.py` then records the current
source hashes instead of refusing a layer that has never been built.

```bash
OFFLINE=1 SDK_CACHE_NO_VERIFY=1 make middleware BOARD=maixcam-sc035hgs
```

The mirror layout under `offline-cache` is owned by the host user while the
build runs as root inside the container; `local-build.sh` therefore allows git
to operate on them regardless of ownership.  When the pins in `versions.env`
move, re-run the staging script to refresh the mirrors.

## Why `BUILDER_BASE_IMAGE` has no digest

It used to be pinned as `debian:sid@sha256:...`.  sid is republished daily and
Docker Hub garbage-collects old manifests within days, so both the original pin
and its replacement eventually failed CI with `debian:sid@sha256:...: not
found`.  A digest pin on a rolling tag cannot stay resolvable, so the base image
is now referenced by tag; what makes a build reproducible is unchanged and
still pinned: `DEBIAN_SNAPSHOT` for the apt layer and the commit SHAs in
`versions.env` for the sources.
