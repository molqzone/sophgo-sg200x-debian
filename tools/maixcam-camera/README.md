# MaixCAM camera viewer

This host tool supports the MaixCAM HEVC stream over USB NCM/RTSP and the
Razver ESP32-C5 H.264 stream over Wi-Fi/RTP. Both paths use a single
slice-threaded low-delay decoder with demuxer buffering disabled.

On Windows, the launcher creates a virtual environment under
`%LOCALAPPDATA%\MaixCAM\camera-viewer`, installs the pinned dependencies only
when `requirements.txt` changes, and opens the viewer:

```powershell
tools\maixcam-camera\run-viewer.ps1
```

Use `-SetupOnly` to prepare or verify the environment without opening a second
RTSP client.

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
/mnt/system/usr/bin/test_mmf 10
```

Wait for `got input signal` before assessing the stream. SC035HGS can take
about a second to deliver its first frame; initial `GetChnFrame` timeouts do
not by themselves mean capture has failed. `Init OK` only confirms that the
configuration was applied. Check that `RecvPic` in `/proc/cvitek/vi` and
`SendOK` in `/proc/cvitek/vpss` keep increasing. The RTSP sample sends a fallback
image without camera input, so opening the RTSP URL alone is not a capture test.

Reboot after updating media or remoteproc kernel modules. The vendor fast-image
driver cannot safely hot-unload after the cmdqu driver's probe has failed.

The SC035HGS sensor captures at 640x480. This preview scales/crops those frames
to 1280x720 at 30 FPS.
