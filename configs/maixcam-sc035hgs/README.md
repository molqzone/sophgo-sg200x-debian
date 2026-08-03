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
markers cannot silently change the sensor identity. No GC4653 ISP parameter blob
is installed for this variant; SC035HGS ISP tuning remains a hardware follow-up.
