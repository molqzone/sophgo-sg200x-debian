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
