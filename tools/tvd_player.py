#!/usr/bin/env python3
"""
tvd_player.py — Python playback tool for the TVD (Tiny Video Delta) format
produced by mp4_to_tvd.py / used by tvd_player.c on ModuOS.

TVD Format (magic 0x21445654 'TVD!'):
    Header (14 bytes, all little-endian):
        uint32_t magic            -> 0x21445654
        uint16_t width
        uint16_t height
        uint16_t fps
        uint16_t total_frames
        uint16_t keyframe_interval
    Frames (total_frames entries), each:
        uint8_t  frame_type       -> 0 = keyframe (full raw RGBA), 1 = delta (XOR mask)
        uint32_t data_size        -> byte count of the zlib-compressed payload (LE)
        uint8_t  data[data_size]  -> zlib-compressed payload
            Keyframe payload: width*height*4 raw RGBA bytes
            Delta payload:    width*height*4 bytes, XOR mask against the
                               previous reconstructed frame's RGBA

Controls (in the preview window):
    Space       — play / pause
    Left/Right  — step one frame backward / forward
    Home (h)    — jump to first frame
    End  (e)    — jump to last frame
    +/-         — speed up / slow down playback (does not change file fps)
    Q / ESC      — quit

Usage:
    python3 tvd_player.py video.tvd
    python3 tvd_player.py video.tvd --scale 2
    python3 tvd_player.py video.tvd --no-gui --info     (just print header info)

Requires: opencv-python (with GUI support, i.e. NOT opencv-python-headless,
          for the --no-gui flag GUI is not needed), numpy
"""

import argparse
import struct
import sys
import zlib

import numpy as np

TVD_MAGIC = 0x21445654
FRAME_KEY = 0
FRAME_DELTA = 1

HEADER_SIZE = 14
FRAME_META_SIZE = 5  # 1 byte type + 4 byte size


class TVDFile:
    """Parses a .tvd file, builds a frame index for O(1) seeking, and
    decodes frames on demand (mirrors tvd_decode_frame / tvd_seek_and_decode
    from tvd_player.c)."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")

        hdr = self.f.read(HEADER_SIZE)
        if len(hdr) < HEADER_SIZE:
            raise ValueError("file too small / truncated header")

        magic, w, h, fps, total_frames, keyint = struct.unpack("<IHHHHH", hdr)
        if magic != TVD_MAGIC:
            raise ValueError(f"not a TVD file (bad magic: 0x{magic:08X})")
        if w == 0 or h == 0 or total_frames == 0 or fps == 0:
            raise ValueError("invalid TVD header (zero field)")

        self.width = w
        self.height = h
        self.fps = fps
        self.total_frames = total_frames
        self.keyframe_interval = keyint
        self.frame_size = w * h * 4

        # Build frame index: (file_offset_of_payload, frame_type, data_size)
        self.index = []
        cur_off = HEADER_SIZE
        for i in range(total_frames):
            self.f.seek(cur_off)
            meta = self.f.read(FRAME_META_SIZE)
            if len(meta) < FRAME_META_SIZE:
                raise ValueError(f"truncated frame index at frame {i}")
            ftype = meta[0]
            dsz = struct.unpack("<I", meta[1:5])[0]
            payload_off = cur_off + FRAME_META_SIZE
            self.index.append((payload_off, ftype, dsz))
            cur_off = payload_off + dsz

        self.frame_buf = bytearray(self.frame_size)      # current decoded frame
        self.prev_frame_buf = bytearray(self.frame_size)  # previous frame (for delta)
        self.last_decoded = -1

    def close(self):
        self.f.close()

    def _decode_single(self, idx):
        """Decode exactly frame `idx`, assuming prev_frame_buf is valid if needed."""
        payload_off, ftype, dsz = self.index[idx]
        self.f.seek(payload_off)
        compressed = self.f.read(dsz)
        if len(compressed) != dsz:
            raise ValueError(f"truncated frame data at frame {idx}")

        raw = zlib.decompress(compressed)
        if len(raw) != self.frame_size:
            raise ValueError(
                f"frame {idx}: decompressed size {len(raw)} != expected "
                f"{self.frame_size}"
            )

        if ftype == FRAME_KEY:
            self.frame_buf[:] = raw
            self.prev_frame_buf[:] = raw
        else:
            cur = np.frombuffer(self.frame_buf, dtype=np.uint8)
            prev = np.frombuffer(self.prev_frame_buf, dtype=np.uint8)
            raw_arr = np.frombuffer(raw, dtype=np.uint8)
            np.bitwise_xor(prev, raw_arr, out=cur)
            self.prev_frame_buf[:] = self.frame_buf

        self.last_decoded = idx

    def seek_and_decode(self, idx):
        """Decode frame `idx`, walking back to the nearest keyframe if needed
        (mirrors tvd_seek_and_decode in tvd_player.c)."""
        idx = max(0, min(idx, self.total_frames - 1))

        if idx == self.last_decoded:
            return idx

        if idx == self.last_decoded + 1 and self.index[idx][1] == FRAME_DELTA:
            self._decode_single(idx)
            return idx

        # Find nearest keyframe at or before idx
        kf = idx
        while kf > 0 and self.index[kf][1] != FRAME_KEY:
            kf -= 1

        for i in range(kf, idx + 1):
            self._decode_single(i)

        return idx

    def get_rgba(self):
        """Return the currently decoded frame as an (H, W, 4) uint8 array."""
        return np.frombuffer(self.frame_buf, dtype=np.uint8).reshape(
            (self.height, self.width, 4)
        )

    def frame_type(self, idx):
        return self.index[idx][1]


def parse_args():
    p = argparse.ArgumentParser(description="Play back a .tvd (Tiny Video Delta) file.")
    p.add_argument("input", help="Path to input .tvd file")
    p.add_argument("--scale", type=float, default=1.0,
                   help="Display scale factor (default: 1.0)")
    p.add_argument("--checker", type=int, default=12,
                   help="Checkerboard cell size in px behind transparent pixels "
                        "(default: 12, 0 = disable)")
    p.add_argument("--no-gui", action="store_true",
                   help="Don't open a display window; just decode/validate the file")
    p.add_argument("--info", action="store_true",
                   help="Print header info and exit")
    p.add_argument("--start", type=int, default=0,
                   help="Frame index to start playback from (default: 0)")
    p.add_argument("--autoplay", action="store_true",
                   help="Start playing immediately instead of paused")
    return p.parse_args()


def make_checker(h, w, cell, color_a=(26, 26, 26), color_b=(42, 42, 42)):
    """Build a BGR checkerboard background image."""
    yy, xx = np.meshgrid(np.arange(h), np.arange(w), indexing="ij")
    mask = ((xx // cell) + (yy // cell)) % 2 == 0
    img = np.empty((h, w, 3), dtype=np.uint8)
    img[mask] = color_a[::-1]
    img[~mask] = color_b[::-1]
    return img


def composite_rgba_over_bgr(rgba, checker_bgr):
    """Alpha-composite an RGBA frame over a checkerboard BGR background,
    returning a BGR image suitable for cv2.imshow."""
    rgb = rgba[:, :, :3].astype(np.float32)
    a = (rgba[:, :, 3:4].astype(np.float32)) / 255.0
    bgr_fg = rgb[:, :, ::-1]
    out = bgr_fg * a + checker_bgr.astype(np.float32) * (1.0 - a)
    return out.astype(np.uint8)


def main():
    args = parse_args()

    try:
        tvd = TVDFile(args.input)
    except (OSError, ValueError) as e:
        sys.exit(f"error: {e}")

    n_key = sum(1 for _, ftype, _ in tvd.index if ftype == FRAME_KEY)
    n_delta = tvd.total_frames - n_key

    print(f"File:              {args.input}")
    print(f"Resolution:        {tvd.width}x{tvd.height}")
    print(f"FPS:               {tvd.fps}")
    print(f"Total frames:      {tvd.total_frames}  ({n_key} key, {n_delta} delta)")
    print(f"Keyframe interval: {tvd.keyframe_interval}")
    print(f"Duration:          {tvd.total_frames / tvd.fps:.2f} s")

    if args.info:
        tvd.close()
        return

    if args.no_gui:
        # Decode every frame once to validate the whole file, no display.
        for i in range(tvd.total_frames):
            tvd.seek_and_decode(i)
        print("Decoded all frames successfully (no errors).")
        tvd.close()
        return

    try:
        import cv2
    except ImportError:
        tvd.close()
        sys.exit("error: this script requires OpenCV for playback. Install with:\n"
                  "    pip install opencv-python\n"
                  "(use --no-gui to validate a file without a display)")

    disp_w = max(1, int(tvd.width * args.scale))
    disp_h = max(1, int(tvd.height * args.scale))

    checker_bgr = None
    if args.checker > 0:
        checker_bgr = make_checker(tvd.height, tvd.width, args.checker)

    win = "TVD Player"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(win, disp_w, disp_h)

    cur_frame = tvd.seek_and_decode(args.start)
    playing = args.autoplay
    fps_override = 0  # 0 = use file fps

    def effective_fps():
        return fps_override if fps_override > 0 else tvd.fps

    print()
    print("Controls: Space=play/pause  Left/Right=step  Home/h=first  End/e=last")
    print("          +/-=speed  Q/ESC=quit")

    last_tick = cv2.getTickCount()
    tick_freq = cv2.getTickFrequency()

    quit_requested = False
    while not quit_requested:
        rgba = tvd.get_rgba()
        if checker_bgr is not None:
            frame_bgr = composite_rgba_over_bgr(rgba, checker_bgr)
        else:
            frame_bgr = cv2.cvtColor(rgba[:, :, :3], cv2.COLOR_RGB2BGR)

        if args.scale != 1.0:
            frame_bgr = cv2.resize(frame_bgr, (disp_w, disp_h),
                                    interpolation=cv2.INTER_NEAREST)

        # OSD overlay
        ftype = tvd.frame_type(cur_frame)
        badge = "KEY" if ftype == FRAME_KEY else "DELTA"
        status = "PLAYING" if playing else "PAUSED"
        text1 = f"{cur_frame}/{tvd.total_frames - 1}  [{badge}]  {effective_fps()} fps  {status}"
        cv2.putText(frame_bgr, text1, (8, 20), cv2.FONT_HERSHEY_SIMPLEX,
                    0.5, (0, 0, 0), 3, cv2.LINE_AA)
        cv2.putText(frame_bgr, text1, (8, 20), cv2.FONT_HERSHEY_SIMPLEX,
                    0.5, (255, 255, 255), 1, cv2.LINE_AA)

        cv2.imshow(win, frame_bgr)

        wait_ms = max(1, int(1000 / max(1, effective_fps())) if playing else 30)
        key = cv2.waitKey(wait_ms) & 0xFF

        if key == ord('q') or key == 27:  # q or ESC
            quit_requested = True
        elif key == ord(' '):
            playing = not playing
        elif key == 81 or key == ord('j'):  # Left arrow (varies by platform) or j
            playing = False
            cur_frame = tvd.seek_and_decode(cur_frame - 1)
        elif key == 83 or key == ord('l'):  # Right arrow or l
            playing = False
            cur_frame = tvd.seek_and_decode(cur_frame + 1)
        elif key == ord('h'):
            playing = False
            cur_frame = tvd.seek_and_decode(0)
        elif key == ord('e'):
            playing = False
            cur_frame = tvd.seek_and_decode(tvd.total_frames - 1)
        elif key == ord('+') or key == ord('='):
            base = fps_override if fps_override > 0 else tvd.fps
            fps_override = min(120, base + 5)
        elif key == ord('-'):
            base = fps_override if fps_override > 0 else tvd.fps
            fps_override = max(1, base - 5)

        # cv2.waitKey on some platforms doesn't return distinct codes for
        # arrow keys reliably; also support extended key codes.
        if key == 2424832:    # Windows VK_LEFT via some backends
            playing = False
            cur_frame = tvd.seek_and_decode(cur_frame - 1)
        elif key == 2555904:  # Windows VK_RIGHT
            playing = False
            cur_frame = tvd.seek_and_decode(cur_frame + 1)

        if playing:
            now = cv2.getTickCount()
            elapsed_ms = (now - last_tick) / tick_freq * 1000.0
            if elapsed_ms >= (1000.0 / max(1, effective_fps())):
                last_tick = now
                nxt = cur_frame + 1
                if nxt >= tvd.total_frames:
                    nxt = 0  # loop
                cur_frame = tvd.seek_and_decode(nxt)

        try:
            if cv2.getWindowProperty(win, cv2.WND_PROP_VISIBLE) < 1:
                break
        except cv2.error:
            break

    cv2.destroyAllWindows()
    tvd.close()


if __name__ == "__main__":
    main()