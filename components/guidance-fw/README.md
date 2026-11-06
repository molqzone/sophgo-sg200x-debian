# guidance-fw component

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
cop status [--contract FILE]   # decode the header; optional contract cross-check
cop dump --offset 0 --bytes 64 # forensics
cop log [--contract FILE]      # console ring
cop format --contract FILE     # the only verb that needs geometry (a cold page has none)
cop watch [--contract FILE]    # new telemetry slots, opaque bytes
cop send --contract FILE --type N --payload HEX
```

So **no `pages.json` is installed into the rootfs, and nothing parses one**:
the runtime source of the addresses is the DT (read through sysfs), the
build-time source is `configs/<board>/memmap.py` (exported as
`SG200X_GUIDANCE_PAGE_BASE` for the firmware build), and the only extra file
`cop` may take is the *product's* contract (`--contract`), which carries what
the header cannot: frame layout, checksum, channel names and command values.
An out-of-tree product build that needs the addresses pins this repository and
imports `memmap.py`; it does not read a copy shipped in the image.

`cop` knows no product names: `send --type N` is the transport-level escape
hatch, and "what the page means" lives in the product's own tooling.

## Contents

```text
firmware/c906-mcu.elf        product RISC-V firmware (remoteproc artifact)
firmware/c906-mcu.bin        raw image, only for an opt-in FSBL/FIP build
firmware/guidance_contract.json  the page layout the firmware was built against
firmware/sha256sums.txt      integrity pins for all three artifacts
ports/osdrv/patches/        0001-cmdqu-allow-user-commands.patch (doorbell)
                            0002-cmdqu-notify-event.patch (notification wait)
tools/rtos-mode              board tool: start the C906L remoteproc at boot
tools/cop                    board tool: page status/dump/log/format, typed commands
linux/systemd/load-systemko.service.d/20-guidance-fw.conf.in
                             boot drop-in template: format the page, then start the
                             rproc in the mode the board's boot policy picks
linux/modprobe.d/guidance-uio.conf   uio_pdrv_genirq of_id=generic-uio
linux/modules-load.d/guidance-uio.conf  load that module at boot
```

## Firmware provenance

The artifacts are built from the product firmware repository
[xrobot-org/bsp-guidance-action](https://github.com/xrobot-org/bsp-guidance-action)
and refreshed by copying `build/firmware/c906-mcu.{elf,bin}` and
`build/firmware/guidance_contract.json` here, then recording new hashes in
`firmware/sha256sums.txt`. The firmware in this revision carries the shared-page
guidance contract at `0x8FFDC000` and answers the Linux doorbell
(`ip_id=IP_SYSTEM, cmd_id=0x7f`) with the mailbox reply sequence mirrored from
the vendor reference implementation.

The page base is the one fact the two sides hand over: build the firmware with the
same value this image declares in `configs/maixcam/memmap.py`, and the ELF, the
linker script and the published contract all follow it.

```sh
cmake --preset c906-release -DSG200X_GUIDANCE_PAGE_BASE=0x8FFDC000
cmake --build build/release --target firmware
# then copy build/firmware/{c906-mcu.elf,c906-mcu.bin,guidance_contract.json} here
# and re-pin: (cd firmware && sha256sum c906-mcu.elf c906-mcu.bin \
#   guidance_contract.json > sha256sums.txt)
```

Board usage:

```sh
sudo cop status                           # self-describing: no contract needed
sudo cop send --contract /lib/firmware/guidance_contract.json --type 1
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

`firmware/guidance_contract.json` is emitted by the firmware build from the same
`User/guidance_contract.hpp` the firmware compiles (`tools/guidance-contract` in
that repository). It carries the page size, every offset, the magic numbers,
the frame geometry, the name-service area, the command values and the console
ring geometry. This component pins it next to the ELF and installs it to
`/lib/firmware/` next to the ELF it describes; the board tools read it there:

* `cop format` / `cop send` take the offsets from the contract (`--contract`);
  `cop status` / `cop dump` need no contract at all because the page is
  self-describing.
* `bootlogd` reads the console ring geometry from it too, so its snapshots cannot
  be silently wrong about where the frozen coprocessor's output is.
* missing or invalid contract = hard error with the path in the message. There is
  no built-in fallback copy: a fallback would hide exactly the drift the contract
  exists to prevent.

The image side keeps the single declaration of the two page addresses in
`configs/maixcam/memmap.py`, because those addresses are the one fact the two
sides must shake hands on. The firmware build is told the same value
(`SG200X_GUIDANCE_PAGE_BASE`) and derives nothing itself: that one number feeds its
linker script, its C++ compile definition and this JSON. `scripts/ci/validate.py`
fails the build unless the declaration, the DTS `guidance-pages` reg, and this
JSON all agree.

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
(`guidance-pages@8ffdb000`, `linux,uio-name = "guidance-pages"`): map 0 is the
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

The drop-in `20-guidance-fw.conf` first runs `/usr/bin/cop format --contract` as an
`ExecStartPre`: the page belongs to Linux (the firmware only validates it), so a
fresh card would otherwise leave the firmware looking at an unformatted page and
silently skipping every publish. It then runs `rtos-mode <mode>`, which loads
`cvitek_mailbox` + `cvitek_remoteproc`, starts the C906L from
`/lib/firmware/c906-mcu.elf`, releases the mailbox IRQ, and then restores the
media chain without stopping the remote processor.

### Boot policy is a board setting, not a build accident

Two settings in `configs/<board>/settings.mk` decide how the C906L starts; both
`scripts/Makefile` and `scripts/ci/validate.py` check them, and the drop-in's
`ExecStart` is generated from them (the committed file is
`20-guidance-fw.conf.in`):

```make
GUIDANCE_FW_LOADER=remoteproc          # remoteproc | fsbl
GUIDANCE_FW_DOORBELL_OWNER=rtos_cmdqu  # rtos_cmdqu | uio
```

The `rtos-mode` argument is not a third setting: it follows from the loader, and
`scripts/addons/guidance-fw/addon.mk` derives it (`remoteproc` → `rtos-mode
remoteproc`, `fsbl` → `rtos-mode attach`) before the install step checks that the
arm exists in the tool. It used to be its own variable, and one build filled it
with `rtos-mode load` -- not a subcommand the tool has, so the unit exited 2 with
the usage text and the core was never released at all.

* `remoteproc` + `rtos_cmdqu` is the production combination: Linux loads the ELF
  and starts the core after it has booted.
* `fsbl` + `rtos_cmdqu` is the debug variant: the loader releases the core and
  Linux only attaches the vendor stack (`rtos-mode attach`, which refuses to load
  or start anything and fails if the core is not already running).
* `uio` as a doorbell owner is rejected outright until it exists: IRQ 101 belongs to
  the vendor `rtos_cmdqu` driver with an exclusive `request_irq`, and
  `uio_pdrv_genirq` cannot share it -- which is also why `fsbl` + `uio` is refused
  (the core would be released before Linux could own the interrupt). The FSBL +
  load pairing -- the DMA race described below -- is no longer expressible at all.

The FSBL never releases the core in the production policy. `scripts/Makefile` hands
fiptool an empty `BLCP_2ND` input, which leaves `BLCP_2ND_RUNADDR` at zero in the
FIP; `bl2_opt.c`'s `load_blcp_2nd()` reads a zero run address as "No C906L image"
and returns without calling `reset_c906l()`. That is deliberate: the core is
started only after Linux has finished booting, because U-Boot and the early
kernel read the SD card over the same shared AXI DMA controller the firmware's
SPI driver uses. Setting `GUIDANCE_FW_LOADER=fsbl` selects the other path (the BIN
is handed to the FSBL and the drop-in runs `rtos-mode attach`); it is retained for
comparison and is known to hang the board after a few minutes.
