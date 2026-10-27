# MaixCAM camera viewer

This host tool supports the MaixCAM HEVC stream over USB NCM/RTSP and the
Razver ESP32-C5 H.264 stream over Wi-Fi/RTP. Both paths use a single
slice-threaded low-delay decoder with demuxer buffering disabled.

On Linux, use the shell launcher, which keeps a virtual environment next to the
script and installs the pinned dependencies only when `requirements.txt`
changes:

```sh
tools/maixcam-camera/run-viewer.sh
```

It needs a display (`ssh -X` if you are remote) and the OpenCV GUI libraries
(`libgl1` on Debian). Every argument is forwarded to `viewer.py`, so
`run-viewer.sh --no-record` or `run-viewer.sh --url rtsp://...` work the same.

On Windows, the launcher creates a virtual environment under
`%LOCALAPPDATA%\MaixCAM\camera-viewer`, installs the pinned dependencies only
when `requirements.txt` changes, and opens the viewer:

```powershell
tools\maixcam-camera\run-viewer.ps1
```

Use `-SetupOnly` to prepare or verify the environment without opening a second
RTSP client.

The viewer records by default. Every run appends the camera's own NAL units to
`tools/maixcam-camera/recordings/usb-<timestamp>.h264`: the RTSP packets are
already Annex-B, so this is a byte-exact copy with no encode cost. The file
opens directly in `mpv`, `vlc` or `ffmpeg -i`, and `ffmpeg -i in.h264 -c copy
out.mp4` turns it into a container if one is wanted.

A red `REC <MiB>` marker is drawn in the corner; `r` stops or starts a new file,
`q` or Escape quits and closes it. Pass `--no-record` to watch without writing,
or `--record-dir <path>` to choose the location. If the marker reads
`REC FAILED`, the console line `recording disabled: ...` has the reason.

A container was tried first and dropped: `add_stream(template=...)` raises on
PyAV 18/19, and a Matroska or MP4 mux would need out-of-band extradata
(`avcC`) that the RTSP path does not provide.

If Python is not on `PATH`, pass its executable explicitly with
`-Python 'C:\path\to\python.exe'`. An existing viewer environment does not need
the original Python executable to be on `PATH`.

The process inherits `HTTP_PROXY` and `HTTPS_PROXY` when package downloads need
a proxy. Pass another stream URL with `-Url`. Close the window, press Escape,
or press `q` to exit.

## Razver ESP32-C5 Wi-Fi

The data path is SC035HGS -> SG2002 Linux H.264 encoder -> shared memory ->
C906L SPI2 -> ESP32-C5 -> Wi-Fi RTP -> this viewer. USB/SSH can be used for
management without carrying the video to the host.

Connect the host to the C5 access point (default `RZ-C5-FPV`) and configure its
Wi-Fi interface as `192.168.4.2/24`. The reference C5 firmware disables DHCP.
Then run:

```powershell
tools\maixcam-camera\run-viewer.ps1 -Transport Razver
```

The included `razver.sdp` listens on UDP 5600 for H.264 RTP payload type 96.
The C5's metadata payload type 98 is not displayed. This mode does not use
`-Url`; the C5 sends RTP directly and does not provide the USB RTSP endpoint.
Open only one RTP receiver on this port. A viewer joining mid-stream may need
to wait for the next SPS/PPS and IDR frame.

The SG2002 requires the matching C906L SPI video firmware and
`sg2002-camera-bridge` from the `bsp-lichee-rvnano-c906l` project. The bridge's
shared-memory header must match the firmware at `0x8ffde000`; this is not the
general SG2002 IPC ring protocol. With the bridge binary installed at
`/usr/local/bin/sg2002-camera-bridge` and `razver-camera.service` installed in
`/etc/systemd/system/`, start the board-side producer with:

```sh
systemctl start razver-camera.service
```

Use `systemctl enable razver-camera.service` to start the producer on later
boots. This was enabled on the development board after the Wi-Fi capture test.

Stop any existing camera application first. To return the camera to the USB
RTSP sample, stop `razver-camera.service` before running `test_mmf 10`.

The service defaults to the reference bridge's conservative encoder settings:
5 FPS and 48 kbit/s. These are configuration targets, not measured delivery
rates. The 2026-09-07 reference firmware delivered about 1.6-1.9 FPS over Wi-Fi:
its SPI transactions are 64 bytes (36 payload bytes), its actual SSI divider
was 4096, and it waits five FreeRTOS ticks after each transaction. Higher frame
rates need SPI/firmware throughput work as well as encoder configuration.
This mode does not claim the USB path's 30 FPS or the sensor's 120 FPS capability.

## USB NCM/RTSP

The tool does not configure the board or ISP. The USB camera service must already
be serving `rtsp://10.42.0.1:8554/live`.

The boot services should report `Result=success`. Check them, then start camera
capture on the board:

```sh
systemctl --no-pager show load-systemko.service sensor-config.service -p Id -p Result
/mnt/data/test_mmf.v3 14        # VI + VENC H.264 + RTSP, follows the sensor mode
/mnt/data/test_mmf.v3 --ispctl manual 100 100
/mnt/data/test_mmf.v3 --ispctl noise save
for m in bnr ynr tnr; do /mnt/data/test_mmf.v3 --ispctl noise set $m 0 24; done
/mnt/data/test_mmf.v3 --ispctl noise set sharpen 0 8
```

Wait for `got input signal` before assessing the stream. SC035HGS can take
about a second to deliver its first frame; initial `GetChnFrame` timeouts do
not by themselves mean capture has failed. `Init OK` only confirms that the
configuration was applied. Check that `RecvPic` in `/proc/cvitek/vi` and
`SendOK` in `/proc/cvitek/vpss` keep increasing. The RTSP sample sends a fallback
image without camera input, so opening the RTSP URL alone is not a capture test.

Reboot after updating media or remoteproc kernel modules. The vendor fast-image
driver cannot safely hot-unload after the cmdqu driver's probe has failed.

Only one process can own the VI device, so stop the guidance application before
starting the sample and stop the sample before starting the application:

```sh
pkill -f guidance_vision_app
pkill -f test_mmf
```

### Do not stop the ISP tuning

`/sys/module/cv181x_vi/parameters/tuning_dis` looks like a free CPU win: with
`1,1,1,1` the `cvitask_isp_pre` thread drops from about 7% to 0.5% and the frame
rate does not change. It is not a win. The pipeline keeps producing frames at
full rate and every one of them is black, so the detector reports zero hits and
the preview goes dark, while the CPU numbers look better. The parameter also
outlives the process, so a stale `1` breaks the next camera process on the same
boot; only a reboot or writing `0,0,0,0` clears it. Keep it at `0,0,0,0`.

The ISP noise-reduction controls (`NRAttr`, `YNRAttr`, `TNRAttr`, sharpen) also
have no measurable effect on this image: setting them to 0, 24, 48, 96 or 160
gives the same high-frequency noise, because the vendor PQ blob that carries the
curve data is the file this middleware refuses to load.

### Preview look versus detection

`test_mmf` starts with the vendor automatic 3A loop, which on this sensor lifts
a dim scene to mid-grey with an analog gain around 3.5x, and the result is the
dense colour speckle this preview used to be blamed for. The detector locks AE
at 100 us and unity gain instead, so the preview only shows what the detector
sees after the `--ispctl manual 100 100` line above. That frame is dark by
design: the ambient scene is suppressed and a beacon stays saturated.

### Sensor mode and geometry

The preview follows the mode named in `/mnt/data/sensor_cfg.ini`; with
`SMS_SC035HGS_MIPI_312P_264FPS_10BIT` the stream is 640x312 at 264 FPS and the
encoder emits H.264 640x312.

The sample sizes its VPSS from the `PIC_SIZE` table, which only knows the full
640x480 SC035HGS geometry. That made the VPSS centre-cut rows 84..395 of a
312-row frame, so the top of the image was missing and the bottom 84 rows were
unwritten memory: the solid green band and the band that appeared to flash.
`sophgo_middleware.c` now takes the size from the ISP public attributes once VI
is up, and `libcvimpp` does the same for the application. If a future mode
change brings the band back, check the `vi size from ISP window` line in the
sample log first.

`before-crop-band.png` is the 84-row band from the centre-cut crop;
`after-crop-band.png` is the same scene and the same manual exposure after the
fix, where every row comes from the sensor.

### The vendor PQ blob

`cvi_sdr_bin_SC035HGS` records the middleware revision it was built against
(`cab880835a2ad5184de5ed7762404b84`) and this middleware is
`9b60189725b5bcb970ec86e4bbdbf600`, so the loader rejected the binary section
and fell back to the JSON inside the same file. That fallback applies the CLUT
half way, which is what produced an all-black frame until the next reboot.

Patch `0018-accept-legacy-sc035hgs-pqbin` adds this bin to the legacy whitelist
that already accepted the `d6db...` bin, with `binPad = 32` because the ISP
section is 173024 bytes where this middleware expects 173056 (the older known
bin needed 248). With it the blob loads: the CLUT errors are gone and the image
is valid.

It is still not installed by default, because the vendor curves are tuned for a
normal exposure. At the 500 us beacon operating point the PQ gamma lifts the
background median from 15 to 38 of 255, which is above the detector's
`threshold_l_min`, and the high-frequency noise goes from 13.6 to 14.7. Install
the file only when a normal-looking picture matters more than background
suppression:

```sh
cp /mnt/cfg/param/cvi_sdr_bin_SC035HGS /mnt/cfg/param/cvi_sdr_bin   # after a reboot
rm /mnt/cfg/param/cvi_sdr_bin                                       # back to JSON
```

The ISP noise-reduction attributes (`NRAttr`, `YNRAttr`, `TNRAttr`, sharpen)
have no measurable effect with either path: 0, 40, 96 and 200 all give the same
spatial and temporal noise, so the noise at this exposure is read noise and the
only lever is more light.
