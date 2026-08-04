"""Interactive low-latency viewer for the MaixCAM HEVC RTSP stream."""

from __future__ import annotations

import argparse
import time

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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", default="rtsp://10.42.0.1:8554/live")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--hide-fps", action="store_true")
    args = parser.parse_args()

    cv2.namedWindow(WINDOW_TITLE, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(WINDOW_TITLE, args.width, args.height)

    try:
        with av.open(
            args.url,
            mode="r",
            options=FORMAT_OPTIONS,
            timeout=(7.0, 3.0),
        ) as container:
            stream = container.streams.video[0]
            configure_decoder(stream)
            previous_arrival = time.perf_counter()

            for frame in container.decode(stream):
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
                cv2.imshow(WINDOW_TITLE, image)
                key = cv2.waitKey(1) & 0xFF
                if key in (27, ord("q")):
                    break
                if cv2.getWindowProperty(WINDOW_TITLE, cv2.WND_PROP_VISIBLE) < 1:
                    break
    finally:
        cv2.destroyAllWindows()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
