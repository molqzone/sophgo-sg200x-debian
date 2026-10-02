# check=skip=InvalidDefaultArgInFrom
# BUILDER_BASE_IMAGE is intentionally required from toolchain.env.
ARG BUILDER_BASE_IMAGE
FROM ${BUILDER_BASE_IMAGE} AS builder

ARG DEBIAN_FRONTEND=noninteractive
ARG HOST_TOOLS_REPO
ARG HOST_TOOLS_COMMIT

# apt comes from live sid, matching both the debian:sid base image and the
# rootfs the image build produces (see scripts/Makefile's mmdebstrap call).
# A dated snapshot here skews against the rolling base within days: apt then
# refuses to install the package set ("... but it is not going to be
# installed"), which is how this previously failed.
RUN test -n "$HOST_TOOLS_REPO" \
    && test -n "$HOST_TOOLS_COMMIT" \
    && rm -f /etc/apt/sources.list.d/* \
    && printf 'deb http://deb.debian.org/debian sid main\n' > /etc/apt/sources.list

RUN apt-get update \
    && apt-get install -y eatmydata \
    && eatmydata apt-get install -y --no-install-recommends \
        arch-test autoconf automake autotools-dev bc binfmt-support bison \
        build-essential ca-certificates ccache cmake cpio curl \
        device-tree-compiler dh-exec dh-make dosfstools e2fsprogs fakeroot \
        fdisk flex gawk gcc-riscv64-unknown-elf gdisk genimage git gnupg \
        gperf joe jq kernel-wedge kmod kpartx libelf-dev libexpat-dev \
        libgmp-dev libmpc-dev libmpfr-dev libssl-dev libtool linux-image-amd64 \
        lz4 mc mmdebstrap mtools ninja-build openssl parted patchutils \
        pkg-config python-is-python3 python3 python3-dev python3-jinja2 \
        python3-setuptools qemu-user-static quilt rsync sbuild \
        sbuild-debian-developer-setup shellcheck sudo swig systemd-container \
        texinfo wget xxd zip zlib1g-dev \
    && git init /host-tools \
    && git -C /host-tools remote add origin "$HOST_TOOLS_REPO" \
    && git -C /host-tools fetch --depth 1 origin "$HOST_TOOLS_COMMIT" \
    && git -C /host-tools checkout --detach FETCH_HEAD \
    && test "$(git -C /host-tools rev-parse HEAD)" = "$HOST_TOOLS_COMMIT" \
    && printf '%s\n' "$HOST_TOOLS_COMMIT" > /host-tools/.pinned-commit \
    && rm -rf /host-tools/gcc/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-elf \
    && rm -rf /host-tools/gcc/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu \
    && rm -rf /host-tools/gcc/gcc-linaro-6.3.1-2017.05-x86_64_arm-linux-gnueabihf \
    && rm -rf /var/lib/apt/lists/*

FROM builder AS build_image
WORKDIR /workspace
CMD ["/bin/bash"]
