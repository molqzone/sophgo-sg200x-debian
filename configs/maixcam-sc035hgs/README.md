# MaixCAM SC035HGS

This image variant inherits the complete MaixCAM platform and replaces only the
camera profile for the SC035HGS adapter.

The routing is based on unpowered continuity checks of the assembled adapter:

- FPC1 pin 2 to FPC2 pin 9: data lane 0 on MIPI RX4
- FPC1 pin 5 to FPC2 pin 11: data lane 1 on MIPI RX3
- FPC1 pin 14 to FPC2 pin 21: MCLK1

The resulting receiver order is clock RX2, data RX4/RX3, with no P/N swap.
The sensor uses I2C4 address `0x30` and the vendor 27 MHz SC035HGS clock mode.

This is a fixed camera profile: every boot restores the SC035HGS configuration
and selects the RX4 plus MCLK1 pinmux, so stale GC4653 data and MaixCAM board
markers cannot silently change the sensor identity. The variant installs the
official Milk-V SC035HGS ISP/PQ data as `/mnt/cfg/param/cvi_sdr_bin`:

- source file: `cvi_sdr_bin_SC035HGS`
- size: `233368` bytes
- SHA256: `249760630718864557f63c94e101e41661fb9278c3350bebd413d66aac6d29a1`

Hardware A/B/A testing on the MaixCAM adapter showed that this data removes the
dense fixed speckle produced by the ISP defaults. With the current middleware,
the PQ MD5 does not match and the loader deliberately falls back to the JSON
section. Camera and ISP startup succeed, but the warning and nonfatal missing
motion/AWB fields remain a compatibility boundary; binary-section compatibility
is not claimed.

The SC035HGS middleware also exposes a process-local runtime noise-control
interface. Save the complete official BNR, YNR, TNR, and sharpen state before
changing one ISO curve entry:

```sh
test_mmf --ispctl noise save
test_mmf --ispctl noise get
test_mmf --ispctl noise set tnr 0 24
test_mmf --ispctl noise restore
```

On the attached MaixCAM adapter at 10 ms, ISO 100, and unity gain, changing only
TNR ISO index 0 from `32` to `24` reduced temporal standard deviation by 4.7%
and frame-difference deviation by 8.3% in a repeated A/B/A capture. Eight
full-frame pattern transitions showed no added visible-frame delay, and the
candidate retained 1280x720 at 30 FPS with zero VI drop or overflow. This severe
transition test does not prove the absence of every localized moving-object
artifact.

The accepted value remains an explicit application-layer override. The official
PQ blob and its SHA256 are unchanged, the snapshot is lost when `test_mmf`
restarts, and `noise restore` returns all captured structures rather than only
the last edited field.
