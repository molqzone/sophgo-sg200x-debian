# Working baselines

One page so two people can name the same state.  Every tag below was flashed and measured
on the board; the measurements are in the tag message of the respective repository.

## `baseline-2026-11-11`

| Component | Repository | Commit |
|---|---|---|
| C906L firmware | `bsp-guidance-action` @ `refactor/shared-page-contract` | `3959800` |
| LL driver | `sgll` (submodule `sg200x-ll-driver`) | `57cae2e` |
| Vision app | `bsp-guidance-vision` @ `main` | `77ebc28` |
| Camera pipeline | `libcvimpp` (submodule) | `ddba481` |
| libxr | submodule of both apps | `9193bd2` |
| Shared page | submodule `shared-page-xr` | `2b3db3b` |
| Image | `sophgo-sg200x-debian` @ `main` | `3de8796` |
| Vendor firmware (baseline.elf) | `components/guidance-fw/firmware/c906-mcu.elf` | `02f36c3a` |

## What this baseline is known to do

- The little core runs a 1 kHz tick from DW Timer4 and **never writes the CLINT**, so
  starting it can no longer freeze the big core (`mtimecmp[0]` is shared: both cores report
  mhartid 0).
- Both sides come up from a power cycle: `guidance-core.timer` at 20 s submits the core
  start through `systemd-run --no-block` (running `rtos-mode` as a job of its own makes the
  systemd queue depend on itself), then `guidance-vision.service` waits for the shared
  page's telemetry head to advance -- the magic alone survives in DDR from the previous
  boot -- and starts the camera, detector and RTSP server.
- Measured after a power cycle: telemetry 20 Hz, IMU 999 Hz, camera 180 fps, detector
  running, `rtsp://<board>:8554/live` serving H.264 plus the metadata track, command frames
  accepted.

## Known problems at this baseline

- The IMU intermittently stops feeding the loop (`png gyro=0`) with **no** `BMI088 wait
  timeout` lines, i.e. the worker thread stops rather than the data-ready interrupt
  missing.  It comes back on a core restart.  This is the next thing to chase.
- The RTSP stream is 320x240 while the detector works on the 640x480 source; the metadata
  track carries `sw`/`sh` so a client can scale the box.

## Rules that came out of the bring-up

1. `/boot/baseline.elf` is loaded **by the boot chain**.  Never put a firmware there that
   has not been verified to survive the boot, or the board cannot come back without
   re-imaging the card.
2. `rtos-mode remoteproc` unloads the camera chain on its way through, so the camera
   modules are reloaded after the core starts.
3. The clock gate for `clk_timer4` is opened in the bring-up (step ⑤, after the SPI reset
   and pads).  Without it every write to the DW timer vanished into an unclocked slave.
