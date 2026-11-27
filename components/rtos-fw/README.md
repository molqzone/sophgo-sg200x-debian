# rtos-fw component

This component ships the product C906L firmware for the SG2002 SBC image. It
replaces the retired `sg2002-ipc` component: the vendor shared-memory IPC
stack and its userspace tools are gone, and what remains is the minimum the
product needs on the small core -- the firmware artifact, the CMDQU doorbell
kernel patches, and the boot wiring.

## `cop`: the generic page manager (and why there is no `pages.json`)

`/usr/bin/cop` is the business-free half of the link. It discovers the pages
from the device tree through sysfs (`/sys/class/uio/*/maps/mapN/{name,addr,size}`,
where `name` is the DT `reg-names` entry), and it reads the *self-describing*
page header for the geometry:

```
cop list                       # pages, maps, addresses, sizes - DT only
cop status                     # decode the header: state, layout, channels
cop dump --offset 0 --bytes 64 # forensics
cop log [--new]                # console ring, via ConsoleDescriptor
cop watch                      # new telemetry slots, opaque bytes
cop send --type N --payload HEX
cop wait-console               # wait for the console descriptor (boot path)
cop wipe-shared --confirm      # cold-page wipe before releasing the core
```

So **no `pages.json` and no `guidance_contract.json` are installed into the
rootfs, and nothing parses one**: the runtime source of the addresses is the DT
(read through sysfs), and the layout comes from the page's own `PageHeader` /
`ConsoleDescriptor`. `cop` has no `--contract` and never includes a product
header; `send --type N` is the transport-level escape hatch, and "what the page
means" lives in the product's own tooling.

The build-time source of the page addresses and size is
`configs/<board>/firmware-view.json` (published from `memmap.py`); the firmware
build reads `SG200X_GUIDANCE_PAGE_BASE` / `SG200X_GUIDANCE_PAGE_BYTES` from it.

## Contents

```text
firmware/c906-mcu.elf        product firmware artifact (remoteproc)
firmware/c906-mcu.bin        same build, raw image, only for an opt-in FSBL/FIP build
firmware/sha256sums.txt      integrity pins for both artifacts
ports/osdrv/patches/        0001-cmdqu-allow-user-commands.patch (doorbell)
                            0002-cmdqu-notify-event.patch (notification wait)
tools/rtos-mode              board tool: start the C906L remoteproc at boot
tools/cop                    generic board tool: page status/dump/log/watch/send,
                             driven only by the page header/console descriptor
tools/deploy_rtos_fw.sh      board tool: install an ELF and reboot into it
tools/deploy.conf            board ownership applied before the core starts
tools/mcu-watchdog.{sh,service}, install-mcu-watchdog.sh
                             board-side core watchdog
tools/board-100boot.sh, board-bootloop.{sh,service}
                             whole-board cold-boot stress
tools/acceptance.sh          delivery acceptance: rate/guidance/doorbell x 4 boots
linux/systemd/rtos-release.service.in
                             the only release unit; after multi-user, rendered
                             from GUIDANCE_FW_LOADER
linux/systemd/load-systemko-none.conf
                             the only load-systemko override: ExecStart=/bin/true
linux/modprobe.d/rtos-uio.conf   uio_pdrv_genirq of_id=generic-uio
linux/modules-load.d/rtos-uio.conf  load that module at boot
```

## Firmware provenance

The **product** artifacts come from the product firmware repository
[molqzone/bsp-guidance-action](https://github.com/molqzone/bsp-guidance-action)
(private; the `xrobot-org/bsp-guidance-action` link this file used to carry never
resolved). They are refreshed by copying `build/firmware/c906-mcu.{elf,bin}` here,
then recording new hashes in `firmware/sha256sums.txt`. The product firmware
carries the shared-page layout at `0x8FFDC000` and answers the Linux doorbell
(`ip_id=IP_SYSTEM, cmd_id=0x7f`) with the mailbox reply sequence mirrored from
the vendor reference implementation.

`firmware/` is the product firmware, not an inert sample: it builds the page,
publishes telemetry and answers the doorbell.  `rtos-fw` owns the artifact, `cop`,
`rtos-mode`, the single release unit (`rtos-release.service`, after multi-user)
and the load-systemko no-op; `guidance-autostart` owns only the vision
application.

The page base/size are the one fact the two sides hand over: build the firmware
with the image's `firmware-view.json`, and the ELF, the linker script and the
runtime page header all follow it.

```sh
# product build: the firmware view is the image's input
cmake --preset c906-release \
  -DSG200X_FIRMWARE_VIEW=/path/to/sophgo-sg200x-debian/configs/maixcam/firmware-view.json
cmake --build build/release --target firmware
# then copy build/firmware/{c906-mcu.elf,c906-mcu.bin} here
# and re-pin: (cd firmware && sha256sum c906-mcu.elf c906-mcu.bin > sha256sums.txt)
```

Board usage:

```sh
sudo cop status                 # self-describing: no side file
sudo cop send --type 4 --payload ''
sudo cop log
```

Commands complete through `RTOS_CMDQU_SEND_WAIT`: the firmware rings mailbox
bank 1, the big core's PLIC line 101 fires, and the kernel wait list wakes the
caller, so nothing polls the mailbox buffer. (The firmware used to ring bank 0,
which has no line to the big core; that was the mailbox interrupt bug.)

Telemetry is event driven in the other direction: after publishing a sample the
firmware rings the reserved notification slot and `guidance-cli watch` blocks in
`RTOS_CMDQU_WAIT_EVENT` (a kernel wait queue over a monotonic sequence). No
polling on either side.

## The page contract: one owner, one artifact

The normative specification is the firmware repo's
`docs/inter-core-protocol.md` (semantics, arithmetic relations, invariants); this
side implements the Linux half of it and consumes the numbers, never re-derives
them.

The layout is owned by the firmware's C++ headers (`GuidanceContract.hpp` and
`shared-page-xr`), and is written into the page itself. This component installs
no side file:

* `cop status` / `dump` / `watch` read the `PageHeader`; `cop send` takes the
  command magic from `PageLayout.command_magic` and writes the generic 8-byte
  `CommandHeader` plus an opaque payload; `cop log` reads the
  `ConsoleDescriptor`.
* `bootlogd` reads the same `ConsoleDescriptor`, so its snapshots cannot be
  silently wrong about where the frozen coprocessor's output is.
* a page that is `UNFORMATTED` / `FOREIGN` / `MISMATCH` is reported and
  refused; there is no fallback layout to hide the drift.

The image side keeps the single declaration of the page base/size in
`configs/maixcam/memmap.py`, published as `firmware-view.json`. The firmware build
is told the same values (`SG200X_GUIDANCE_PAGE_BASE` / `SG200X_GUIDANCE_PAGE_BYTES`)
and derives nothing itself. `scripts/ci/validate.py` fails the build unless the
view, `memmap.py` and the DTS `rtos-pages` reg all agree.

## Kernel patches

Both patches are applied to the OSdrv tree in `scripts/Makefile` when
`ENABLE_GUIDANCE_FW=1` in the board settings, and each one leaves `grep` markers
behind so a silently skipped patch fails the build instead of shipping.

`0001-cmdqu-allow-user-commands.patch` keeps the vendor `rtos_cmdqu` driver
application-agnostic: it allows the full 7-bit command space for `IP_SYSTEM`
(so the `0x7f` doorbell passes validation) and serializes synchronous
transactions.

`0002-cmdqu-notify-event.patch` adds the firmware-to-Linux notification, which
the vendor driver cannot express: its handler wakes a `SEND_WAIT` only when
`(ip_id, cmd_id)` match, so a notification answering no pending request is
acknowledged and dropped. Mailbox slot 7 is reserved as that notification (it
carries no context -- the data is in the shared page), the handler counts it and
wakes a dedicated wait queue, and `RTOS_CMDQU_WAIT_EVENT` blocks until the
counter moves past the value the caller last saw. Linux never allocates the
reserved slot for a command, so a ring cannot zero a command parked in the same
context word.

The slot -- rather than a second driver -- is deliberate: the vendor module's
`request_irq()` is exclusive and `uio_pdrv_genirq` does not share interrupts, so
IRQ 101 has exactly one owner and the notification has to ride on it.

## Userspace mapping (UIO, not /dev/mem)

The board DTS declares both pages as one `generic-uio` node
(`rtos-pages@8ffdb000`, `linux,uio-name = "rtos-pages"`): map 0 is the
console page, map 1 the shared page.  `uio_pdrv_genirq` is built as a module and
matched through `of_id=generic-uio`; `modules-load.d` loads it at boot so
`/dev/uioN` exists before anything reads the pages.  The node carries **no
interrupt** -- mailbox IRQ 101 belongs to `cv181x_rtos_cmdqu` alone, and the
firmware notification rides its `RTOS_CMDQU_WAIT_EVENT` ioctl.

`uio` maps `UIO_MEM_PHYS` with `pgprot_noncached`, so userspace sees the same
coherency domain `/dev/mem` used to provide, without the open physical-memory
window: `cop`, `bootlogd` and `guidance-cli` all read the pages
through `/dev/uioN`.

## Boot order

`rtos-release.service` runs synchronously after `multi-user.target` (all mounts
and services are up), calls `rtos-mode remoteproc`, which loads
`cvitek_mailbox` + `cvitek_remoteproc`, starts the C906L from
`/lib/firmware/c906-mcu.elf`, releases the mailbox IRQ, and then restores the
media chain without stopping the remote processor.  Before that media load it
waits for the firmware console descriptor (its magic/offsets are read from the
page by `cop wait-console`): `remoteproc running` is not "firmware ready", and
loading `cv181x_base` while the S-mode bring-up is still configuring clocks can
wedge the SoC.  The vendor `load-systemko` hook is installed as a no-op so it
cannot release a second time.  It does
**not** format the page: the core that writes the page builds it at bring-up,
and `cop status` reports the state instead of inventing one.

### Boot policy is a board setting, not a build accident

Three settings in `configs/<board>/settings.mk` decide how the C906L starts; both
`scripts/Makefile` and `scripts/ci/validate.py` check them against the board
settings and DTS; the release unit runs the production `remoteproc` command:

```make
GUIDANCE_FW_LOADER=remoteproc        # remoteproc | fsbl
GUIDANCE_FW_RPROC_MODE=load          # load | attach
GUIDANCE_FW_DOORBELL_OWNER=rtos_cmdqu  # rtos_cmdqu | uio
```

* `remoteproc` + `load` + `rtos_cmdqu` is the production combination: Linux loads
  the ELF and starts the core after it has booted.
* `fsbl` + `attach` + `rtos_cmdqu` is the debug variant: the loader releases the
  core and Linux only attaches the vendor stack (`rtos-mode attach`, which refuses
  to load or start anything and fails if the core is not already running).
* `uio` as a doorbell owner is rejected outright until it exists: IRQ 101 belongs to
  the vendor `rtos_cmdqu` driver with an exclusive `request_irq`, and
  `uio_pdrv_genirq` cannot share it -- which is also why `fsbl` + `uio` is refused
  (the core would be released before Linux could own the interrupt). `load` + `fsbl`
  is refused as the DMA race described below.

The FSBL never releases the core in the production policy. `scripts/Makefile` hands
fiptool an empty `BLCP_2ND` input, which leaves `BLCP_2ND_RUNADDR` at zero in the
FIP; `bl2_opt.c`'s `load_blcp_2nd()` reads a zero run address as "No C906L image"
and returns without calling `reset_c906l()`. That is deliberate: the core is
started only after Linux has finished booting, because U-Boot and the early
kernel read the SD card over the same shared AXI DMA controller the firmware's
SPI driver uses. The `fsbl` path (BIN handed to the FSBL, Linux attaches) is retained for debug
comparison only and is known to hang the board after a few minutes.
