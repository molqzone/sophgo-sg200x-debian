# guidance-fw component

This component ships the product C906L firmware for the SG2002 SBC image. It
replaces the retired `sg2002-ipc` component: the vendor shared-memory IPC
stack and its userspace tools are gone, and what remains is the minimum the
product needs on the small core -- the firmware artifact, the CMDQU doorbell
kernel patch, and the boot wiring.

## Contents

```text
firmware/c906-mcu.elf        product RISC-V firmware (remoteproc artifact)
firmware/c906-mcu.bin        raw image, only for an opt-in FSBL/FIP build
firmware/sha256sums.txt      integrity pins for the two artifacts
ports/osdrv/patches/        0001-cmdqu-allow-user-commands.patch (doorbell)
tools/rtos-mode              board tool: start the C906L remoteproc at boot
tools/guidance-cmd.py        board tool: ping / led commands over the doorbell
linux/systemd/load-systemko.service.d/20-guidance-fw.conf
                             boot drop-in: rproc start before the media chain
```

## Firmware provenance

The artifacts are built from the product firmware repository
[xrobot-org/bsp-guidance-action](https://github.com/xrobot-org/bsp-guidance-action)
and refreshed by copying `build/firmware/c906-mcu.{elf,bin}` here and recording
new hashes in `firmware/sha256sums.txt`. The firmware in this revision carries
the shared-page guidance contract at `0x8FFDC000` and answers the Linux
doorbell (`ip_id=IP_SYSTEM, cmd_id=0x7f`) with the mailbox reply sequence
mirrored from the vendor reference implementation.

Board usage:

```sh
sudo python3 /usr/local/bin/guidance-cmd.py ping
sudo python3 /usr/local/bin/guidance-cmd.py led on
sudo python3 /usr/local/bin/guidance-cmd.py status
```

The reply is polled from the mailbox buffer rather than completing through
`RTOS_CMDQU_SEND_WAIT`: on this image the ca53-side mailbox interrupt does not
deliver, so the kernel wait list can never be woken. The bytes are latched and
readable, so the tool polls them.

## Kernel patch

`0001-cmdqu-allow-user-commands.patch` keeps the vendor `rtos_cmdqu` driver
application-agnostic: it allows the full 7-bit command space for `IP_SYSTEM`
(so the `0x7f` doorbell passes validation) and serializes synchronous
transactions. The patch is applied to the OSdrv tree when
`ENABLE_GUIDANCE_FW=1` in the board settings.

## Boot order

`load-systemko.service` runs `rtos-mode remoteproc`, which loads
`cvitek_mailbox` + `cvitek_remoteproc`, starts the C906L from
`/lib/firmware/c906-mcu.elf`, releases the mailbox IRQ, and then restores the
media chain without stopping the remote processor.

The FSBL never releases the core on this path. `scripts/Makefile` hands fiptool
an empty `BLCP_2ND` input, which leaves `BLCP_2ND_RUNADDR` at zero in the FIP;
`bl2_opt.c`'s `load_blcp_2nd()` reads a zero run address as "No C906L image"
and returns without calling `reset_c906l()`. That is deliberate: the core is
started only after Linux has finished booting, because U-Boot and the early
kernel read the SD card over the same shared AXI DMA controller the firmware's
SPI driver uses. Setting `FSBL_BLCP_2ND_PATH` back to `c906-mcu.bin` restores
the FSBL boot path, which is retained only for comparison and is known to hang
the board after a few minutes.
