"""View MaixCAM over USB RTSP or the Razver ESP32-C5 RTP bridge.

The stream is recorded as it arrives, by default, into `recordings/` next to this
script: video packets are remuxed into Matroska without re-encoding, so the file
is the camera's own H.264 bitstream and recording costs nothing beyond the disk
write.  Press `r` to start a new file or stop, `q` or Escape to quit.
"""

from __future__ import annotations

import argparse
import time
from datetime import datetime
from pathlib import Path

import av
import cv2


WINDOW_TITLE = "MaixCAM"
FORMAT_OPTIONS = {
    "rtsp_transport": "tcp",
    "fflags": "nobuffer",
    "flags": "low_delay",
    "max_delay": "0",
    "reorder_queue_size": "0",
}


def configure_decoder(stream: av.VideoStream) -> None:
    stream.codec_context.thread_count = 1
    stream.codec_context.thread_type = av.codec.context.ThreadType.SLICE
    stream.codec_context.flags |= av.codec.context.Flags.low_delay


class Recorder:
    """Appends the camera's NAL units to a raw Annex-B file.

    The packets RTSP delivers are already Annex-B (start-code prefixed), so
    concatenating them is a lossless copy of what the board sent.  A container
    was tried first and rejected: `add_stream(template=...)` raises on this PyAV,
    and a Matroska/MP4 mux would need out-of-band extradata the RTSP path does
    not provide.  A raw .h264 opens directly in mpv, VLC and ffmpeg.
    """

    def __init__(self, directory: Path, template: av.VideoStream, prefix: str) -> None:
        self._directory = directory
        self._template = template
        self._prefix = prefix
        self._file = None
        self._path: Path | None = None
        self._packets = 0
        self._bytes = 0
        self.error: str | None = None

    @property
    def active(self) -> bool:
        return self._file is not None

    @property
    def path(self) -> Path | None:
        return self._path

    @property
    def packets(self) -> int:
        return self._packets

    @property
    def megabytes(self) -> float:
        return self._bytes / (1024.0 * 1024.0)

    def start(self) -> Path | None:
        self.stop()
        self.error = None
        try:
            self._directory.mkdir(parents=True, exist_ok=True)
            stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
            path = self._directory / f"{self._prefix}-{stamp}.h264"
            self._file = open(path, "wb")  # noqa: SIM115 - closed in stop()
        except Exception as error:  # noqa: BLE001 - the preview must keep running
            self.error = str(error)
            print(f"recording disabled: {error}", flush=True)
            return None
        self._path = path
        self._packets = 0
        self._bytes = 0
        print(f"recording {path}", flush=True)
        return path

    def write(self, packet: av.Packet) -> None:
        if self._file is None:
            return
        try:
            data = bytes(packet)
            self._file.write(data)
            self._bytes += len(data)
            self._packets += 1
        except Exception as error:  # noqa: BLE001
            print(f"recording stopped after {self._packets} packets: {error}", flush=True)
            self.stop()

    def stop(self) -> None:
        if self._file is not None:
            try:
                self._file.close()
            except Exception:  # noqa: BLE001
                pass
            if self._path is not None:
                print(
                    f"saved {self._path} ({self._packets} packets, "
                    f"{self.megabytes:.1f} MiB)",
                    flush=True,
                )
        self._file = None
        self._path = None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--transport", choices=("usb", "razver"), default="usb")
    parser.add_argument("--url", default="rtsp://10.42.0.1:8554/live")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--hide-fps", action="store_true")
    parser.add_argument(
        "--record-dir",
        type=Path,
        default=Path(__file__).with_name("recordings"),
        help="where the recordings go",
    )
    parser.add_argument(
        "--no-record",
        action="store_true",
        help="open the viewer without recording",
    )
    args = parser.parse_args()

    source = args.url
    options = dict(FORMAT_OPTIONS)
    stream_timeout = (7.0, 3.0)
    window_title = f"{WINDOW_TITLE} - USB RTSP"
    prefix = "usb"
    if args.transport == "razver":
        source = str(Path(__file__).with_name("razver.sdp"))
        options.pop("rtsp_transport")
        options["protocol_whitelist"] = "file,udp,rtp"
        # A receiver joining an RTP broadcast must wait for the next SPS/PPS
        # and IDR. The current 64-byte SPI bridge can have a long GOP interval.
        stream_timeout = (45.0, 10.0)
        window_title = f"{WINDOW_TITLE} - ESP32-C5 Wi-Fi"
        prefix = "razver"

    print(f"Opening {window_title}: {source}", flush=True)
    print(f"recordings go to {args.record_dir}", flush=True)
    cv2.namedWindow(window_title, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(window_title, args.width, args.height)

    recorder: Recorder | None = None
    try:
        with av.open(
            source,
            mode="r",
            options=options,
            timeout=stream_timeout,
        ) as container:
            stream = container.streams.video[0]
            configure_decoder(stream)
            recorder = Recorder(args.record_dir, stream, prefix)
            if not args.no_record:
                recorder.start()
            previous_arrival = time.perf_counter()

            # Demux, so the same packets can be both displayed and recorded.
            for packet in container.demux(stream):
                if recorder.active:
                    recorder.write(packet)
                for frame in packet.decode():
                    now = time.perf_counter()
                    image = frame.to_ndarray(format="bgr24")
                    if not args.hide_fps:
                        fps = 1.0 / max(now - previous_arrival, 1e-6)
                        cv2.putText(
                            image,
                            f"{fps:4.1f} FPS",
                            (16, 32),
                            cv2.FONT_HERSHEY_SIMPLEX,
                            0.7,
                            (0, 255, 0),
                            2,
                            cv2.LINE_AA,
                        )
                    previous_arrival = now
                    if recorder.active and recorder.path is not None:
                        label = f"REC {recorder.megabytes:5.1f}M"
                        colour = (0, 0, 255)
                    elif recorder.error:
                        label = "REC FAILED"
                        colour = (0, 165, 255)
                    else:
                        label = "NO REC"
                        colour = (128, 128, 128)
                    cv2.putText(
                        image,
                        label,
                        (image.shape[1] - 150, 32),
                        cv2.FONT_HERSHEY_SIMPLEX,
                        0.7,
                        colour,
                        2,
                        cv2.LINE_AA,
                    )
                    cv2.imshow(window_title, image)
                    key = cv2.waitKey(1) & 0xFF
                    if key in (27, ord("q")):
                        return 0
                    if key == ord("r"):
                        if recorder.active:
                            recorder.stop()
                        else:
                            recorder.start()
                    if cv2.getWindowProperty(window_title, cv2.WND_PROP_VISIBLE) < 1:
                        return 0
    finally:
        if recorder is not None:
            recorder.stop()
        cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
