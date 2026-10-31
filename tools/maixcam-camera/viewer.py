"""View MaixCAM over USB RTSP or the Razver ESP32-C5 RTP bridge.

The window is PyQt6, not OpenCV's imshow: OpenCV ships Qt plugins for xcb only,
so on a Wayland session imshow aborts before it can draw anything, and PyQt6
brings its own Qt with both platforms.

The stream is recorded by default into `recordings/` next to this script: the
RTSP packets are already Annex-B, so they are copied byte for byte into a .h264
file with no re-encoding.  Press `r` to start a new file or stop, `q` or Escape
to quit.
"""

from __future__ import annotations

import argparse
import sys
import threading
import time
from datetime import datetime
from fractions import Fraction
from pathlib import Path

import av
from PyQt6 import QtCore, QtGui, QtWidgets


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

    A container was tried first and dropped: `add_stream(template=...)` raises on
    PyAV 18/19, and a Matroska or MP4 mux would need out-of-band extradata
    (`avcC`) that the RTSP path does not provide.  A raw .h264 opens directly in
    mpv, VLC and ffmpeg.
    """

    def set_frame_size(self, width: int, height: int) -> None:
        self._width = width
        self._height = height

    def __init__(self, directory: Path, prefix: str, fmt: str = "mp4",
                 fps: int = 65) -> None:
        self._directory = directory
        self._prefix = prefix
        self._format = fmt
        self._fps = fps
        self._width = 640
        self._height = 312
        self._stream = None
        self._pts = 0
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
    def megabytes(self) -> float:
        return self._bytes / (1024.0 * 1024.0)

    def start(self) -> Path | None:
        self.stop()
        self.error = None
        try:
            self._directory.mkdir(parents=True, exist_ok=True)
            stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
            if self._format == "mp4":
                path = self._directory / f"{self._prefix}-{stamp}.mp4"
                container = av.open(str(path), mode="w", format="mp4")
                stream = container.add_stream("libx264", rate=self._fps)
                stream.width = self._width
                stream.height = self._height
                stream.pix_fmt = "yuv420p"
                stream.options = {"preset": "ultrafast", "crf": "23"}
                self._container = container
                self._stream = stream
                self._pts = 0
            else:
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

    def write_frame(self, frame: av.VideoFrame) -> None:
        """Encode one decoded frame; used by the mp4 mode."""
        if self._container is None or self._stream is None:
            return
        try:
            frame.pts = self._pts
            frame.time_base = Fraction(1, self._fps)
            self._pts += 1
            self._bytes += frame.width * frame.height * 3 // 2
            for packet in self._stream.encode(frame):
                self._container.mux(packet)
        except Exception as error:  # noqa: BLE001
            print(f"recording stopped at frame {self._pts}: {error}", flush=True)
            self.stop()

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
        if self._container is not None and self._stream is not None:
            try:
                for packet in self._stream.encode(None):
                    self._container.mux(packet)
            except Exception:  # noqa: BLE001
                pass
            try:
                self._container.close()
            except Exception:  # noqa: BLE001
                pass
            if self._path is not None:
                print(f"saved {self._path} ({self._pts} frames)", flush=True)
            self._container = None
            self._stream = None
            self._path = None
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


class StreamWorker(QtCore.QThread):
    """Demuxes the stream, keeps only the newest frame, and records packets."""

    frame_ready = QtCore.pyqtSignal()
    failed = QtCore.pyqtSignal(str)
    opened = QtCore.pyqtSignal(int, int)

    def __init__(self, source: str, options: dict, timeout: tuple, recorder: Recorder) -> None:
        super().__init__()
        self._source = source
        self._options = options
        self._timeout = timeout
        self._recorder = recorder
        self._lock = threading.Lock()
        self._image: QtGui.QImage | None = None
        self._fps = 0.0

    def latest(self) -> QtGui.QImage | None:
        with self._lock:
            return self._image

    def fps(self) -> float:
        with self._lock:
            return self._fps

    def run(self) -> None:  # noqa: C901 - one flat loop reads better here
        try:
            container = av.open(
                self._source,
                mode="r",
                options=self._options,
                timeout=self._timeout,
            )
        except Exception as error:  # noqa: BLE001
            self.failed.emit(str(error))
            return

        with container:
            stream = container.streams.video[0]
            configure_decoder(stream)
            self.opened.emit(stream.codec_context.width, stream.codec_context.height)
            previous = time.perf_counter()
            try:
                for packet in container.demux(stream):
                    if self.isInterruptionRequested():
                        break
                    if self._recorder.active:
                        if self._recorder.mode == "h264":
                            self._recorder.write(packet)
                    for frame in packet.decode():
                        if self._recorder.active and self._recorder.mode == "mp4":
                            self._recorder.write_frame(frame)
                        now = time.perf_counter()
                        fps = 1.0 / max(now - previous, 1e-6)
                        previous = now
                        # Convert once, here: the GUI only copies the newest
                        # image, so a slow repaint cannot make a queue grow.
                        array = frame.to_ndarray(format="bgr24")
                        image = QtGui.QImage(
                            array.data,
                            array.shape[1],
                            array.shape[0],
                            array.strides[0],
                            QtGui.QImage.Format.Format_BGR888,
                        ).copy()
                        with self._lock:
                            self._image = image
                            self._fps = fps
                        self.frame_ready.emit()
            except Exception as error:  # noqa: BLE001
                self.failed.emit(str(error))


class Viewer(QtWidgets.QWidget):
    def __init__(self, worker: StreamWorker, recorder: Recorder, title: str,
                 show_fps: bool) -> None:
        super().__init__()
        self._worker = worker
        self._recorder = recorder
        self._show_fps = show_fps
        self._image: QtGui.QImage | None = None
        self.setWindowTitle(title)
        self.resize(1280, 720)
        self.setFocusPolicy(QtCore.Qt.FocusPolicy.StrongFocus)
        worker.frame_ready.connect(self._on_frame)
        worker.opened.connect(self._on_opened)
        worker.failed.connect(self._on_failed)

    @QtCore.pyqtSlot()
    def _on_frame(self) -> None:
        self._image = self._worker.latest()
        self.update()

    @QtCore.pyqtSlot(int, int)
    def _on_opened(self, width: int, height: int) -> None:
        print(f"connected: {width}x{height}", flush=True)

    @QtCore.pyqtSlot(str)
    def _on_failed(self, message: str) -> None:
        print(f"stream error: {message}", flush=True)
        QtWidgets.QMessageBox.critical(self, "MaixCAM", message)
        self.close()

    def paintEvent(self, event: QtGui.QPaintEvent) -> None:  # noqa: N802 - Qt API
        del event
        painter = QtGui.QPainter(self)
        painter.fillRect(self.rect(), QtCore.Qt.GlobalColor.black)
        if self._image is None:
            painter.setPen(QtCore.Qt.GlobalColor.lightGray)
            painter.drawText(self.rect(), QtCore.Qt.AlignmentFlag.AlignCenter,
                             "connecting...")
            return

        image = self._image
        scaled = image.size().scaled(self.size(), QtCore.Qt.AspectRatioMode.KeepAspectRatio)
        target = QtCore.QRect(QtCore.QPoint(0, 0), scaled)
        target.moveCenter(self.rect().center())
        painter.setRenderHint(QtGui.QPainter.RenderHint.SmoothPixmapTransform, True)
        painter.drawImage(target, image)

        font = painter.font()
        font.setPointSize(12)
        font.setBold(True)
        painter.setFont(font)
        if self._show_fps:
            painter.setPen(QtGui.QColor(0, 255, 0))
            painter.drawText(target.adjusted(12, 8, 0, 0),
                             QtCore.Qt.AlignmentFlag.AlignLeft,
                             f"{self._worker.fps():5.1f} FPS")
        if self._recorder.active:
            painter.setPen(QtGui.QColor(255, 0, 0))
            text = f"REC {self._recorder.megabytes:5.1f}M"
        elif self._recorder.error:
            painter.setPen(QtGui.QColor(255, 165, 0))
            text = "REC FAILED"
        else:
            painter.setPen(QtGui.QColor(128, 128, 128))
            text = "NO REC"
        painter.drawText(target.adjusted(0, 8, -12, 0),
                         QtCore.Qt.AlignmentFlag.AlignRight, text)

    def keyPressEvent(self, event: QtGui.QKeyEvent) -> None:  # noqa: N802 - Qt API
        key = event.key()
        if key in (QtCore.Qt.Key.Key_Q, QtCore.Qt.Key.Key_Escape):
            self.close()
            return
        if key == QtCore.Qt.Key.Key_R:
            if self._recorder.active:
                self._recorder.stop()
            else:
                self._recorder.start()
            self.update()
            return
        super().keyPressEvent(event)

    def closeEvent(self, event: QtGui.QCloseEvent) -> None:  # noqa: N802 - Qt API
        self._worker.requestInterruption()
        self._worker.wait(2000)
        self._recorder.stop()
        event.accept()


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
        "--record-format",
        choices=("mp4", "h264"),
        default="mp4",
        help="mp4 re-encodes to a playable file; h264 keeps the exact bitstream",
    )
    parser.add_argument("--fps", type=int, default=65, help="timestamps for the mp4")
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

    application = QtWidgets.QApplication(sys.argv)
    recorder = Recorder(args.record_dir, prefix, args.record_format, args.fps)
    if not args.no_record:
        recorder.start()
    worker = StreamWorker(source, options, stream_timeout, recorder)
    viewer = Viewer(worker, recorder, window_title, not args.hide_fps)
    viewer.resize(args.width, args.height)
    viewer.show()
    worker.start()
    return application.exec()


if __name__ == "__main__":
    raise SystemExit(main())
