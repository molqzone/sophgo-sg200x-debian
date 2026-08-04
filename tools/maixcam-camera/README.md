# MaixCAM low-latency viewer

This host tool displays the MaixCAM HEVC stream without the large frame queue
observed in generic OpenCV and VLC defaults. It uses TCP RTSP with demuxer
buffering disabled and a single slice-threaded low-delay decoder.

On Windows, the launcher creates a virtual environment under
`%LOCALAPPDATA%\MaixCAM\camera-viewer`, installs the pinned dependencies only
when `requirements.txt` changes, and opens the viewer:

```powershell
tools\maixcam-camera\run-viewer.ps1
```

Use `-SetupOnly` to prepare or verify the environment without opening a second
RTSP client.

The process inherits `HTTP_PROXY` and `HTTPS_PROXY` when package downloads need
a proxy. Pass another stream URL with `-Url`. Close the window, press Escape,
or press `q` to exit.

The tool does not configure the board or ISP. The camera service must already
be serving `rtsp://10.42.0.1:8554/live`.
