#!/usr/bin/env python3
"""
mp4_to_tvd.py — Convert an MP4 (or any OpenCV-readable video) into the
TVD (Tiny Video Delta) format used by tvd_player.c on ModuOS.

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
            Delta payload:    width*height*4 bytes, XOR of this frame's RGBA
                               against the previous frame's RGBA (post-XOR
                               reconstruction), pre-compression.

Usage:
    python3 mp4_to_tvd.py input.mp4 output.tvd
    python3 mp4_to_tvd.py input.mp4 output.tvd --width 640 --height 360
    python3 mp4_to_tvd.py input.mp4 output.tvd --fps 24 --keyint 30
    python3 mp4_to_tvd.py input.mp4 output.tvd --max-frames 300 --level 9

Requires: opencv-python (or opencv-python-headless), numpy
"""

import argparse
import struct
import sys
import zlib

import numpy as np

try:
    import cv2
except ImportError:
    sys.exit("error: this script requires OpenCV. Install with:\n"
              "    pip install opencv-python-headless")

TVD_MAGIC = 0x21445654
FRAME_KEY = 0
FRAME_DELTA = 1


def parse_args():
    p = argparse.ArgumentParser(description="Convert MP4 video to TVD format.")
    p.add_argument("input", help="Path to input video (e.g. input.mp4)")
    p.add_argument("output", help="Path to output .tvd file")
    p.add_argument("--width", type=int, default=0,
                   help="Resize output width (default: keep source width)")
    p.add_argument("--height", type=int, default=0,
                   help="Resize output height (default: keep source height)")
    p.add_argument("--fps", type=float, default=0,
                   help="Override output fps (default: use source fps, rounded)")
    p.add_argument("--keyint", type=int, default=30,
                   help="Keyframe interval — force a full keyframe every N frames "
                        "(default: 30)")
    p.add_argument("--max-frames", type=int, default=0,
                   help="Limit total number of frames encoded (0 = no limit)")
    p.add_argument("--level", type=int, default=6,
                   help="zlib compression level 0-9 (default: 6)")
    p.add_argument("--alpha", type=int, default=255,
                   help="Alpha channel value to bake into every pixel, 0-255 "
                        "(default: 255, fully opaque)")
    return p.parse_args()


def bgr_to_rgba(frame_bgr, alpha):
    """Convert an OpenCV BGR frame to a packed RGBA byte buffer."""
    h, w = frame_bgr.shape[:2]
    rgba = np.empty((h, w, 4), dtype=np.uint8)
    rgba[:, :, 0] = frame_bgr[:, :, 2]  # R
    rgba[:, :, 1] = frame_bgr[:, :, 1]  # G
    rgba[:, :, 2] = frame_bgr[:, :, 0]  # B
    rgba[:, :, 3] = alpha               # A
    return rgba.tobytes()


def main():
    args = parse_args()

    if not (0 <= args.alpha <= 255):
        sys.exit("error: --alpha must be between 0 and 255")
    if not (0 <= args.level <= 9):
        sys.exit("error: --level must be between 0 and 9")
    if args.keyint < 1:
        sys.exit("error: --keyint must be >= 1")

    cap = cv2.VideoCapture(args.input)
    if not cap.isOpened():
        sys.exit(f"error: could not open input video: {args.input}")

    src_w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    src_h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    src_fps = cap.get(cv2.CAP_PROP_FPS)
    if src_fps <= 0 or src_fps != src_fps:  # NaN guard
        src_fps = 30.0

    out_w = args.width if args.width > 0 else src_w
    out_h = args.height if args.height > 0 else src_h
    out_fps = int(round(args.fps if args.fps > 0 else src_fps))
    out_fps = max(1, min(out_fps, 65535))

    if out_w <= 0 or out_h <= 0:
        sys.exit("error: could not determine video dimensions")
    if out_w > 65535 or out_h > 65535:
        sys.exit("error: width/height must fit in uint16 (<= 65535)")

    needs_resize = (out_w != src_w) or (out_h != src_h)

    print(f"Input:  {args.input}")
    print(f"Source: {src_w}x{src_h} @ {src_fps:.3f} fps")
    print(f"Output: {out_w}x{out_h} @ {out_fps} fps, keyframe_interval={args.keyint}")

    frame_size = out_w * out_h * 4
    prev_rgba = None
    frame_records = []  # list of (frame_type, compressed_bytes)
    frame_idx = 0

    while True:
        if args.max_frames and frame_idx >= args.max_frames:
            break

        ok, frame_bgr = cap.read()
        if not ok:
            break

        if needs_resize:
            frame_bgr = cv2.resize(frame_bgr, (out_w, out_h),
                                    interpolation=cv2.INTER_AREA)

        rgba = bgr_to_rgba(frame_bgr, args.alpha)

        is_keyframe = (frame_idx % args.keyint == 0) or (prev_rgba is None)

        if is_keyframe:
            payload = rgba
            ftype = FRAME_KEY
        else:
            cur_arr = np.frombuffer(rgba, dtype=np.uint8)
            prev_arr = np.frombuffer(prev_rgba, dtype=np.uint8)
            xor_arr = np.bitwise_xor(cur_arr, prev_arr)
            payload = xor_arr.tobytes()
            ftype = FRAME_DELTA

        compressed = zlib.compress(payload, args.level)
        frame_records.append((ftype, compressed))

        prev_rgba = rgba
        frame_idx += 1

        if frame_idx % 50 == 0:
            print(f"  encoded {frame_idx} frames...")

    cap.release()

    total_frames = len(frame_records)
    if total_frames == 0:
        sys.exit("error: no frames were read from input video")
    if total_frames > 65535:
        sys.exit(f"error: total_frames ({total_frames}) exceeds uint16 max (65535); "
                  f"use --max-frames to limit")

    print(f"Encoded {total_frames} frames. Writing {args.output}...")

    with open(args.output, "wb") as f:
        # Header: magic, width, height, fps, total_frames, keyframe_interval
        f.write(struct.pack("<IHHHHH",
                             TVD_MAGIC,
                             out_w,
                             out_h,
                             out_fps,
                             total_frames,
                             args.keyint))

        key_count = 0
        for ftype, compressed in frame_records:
            f.write(struct.pack("<B", ftype))
            f.write(struct.pack("<I", len(compressed)))
            f.write(compressed)
            if ftype == FRAME_KEY:
                key_count += 1

    import os
    out_size = os.path.getsize(args.output)
    print(f"Done. {key_count} keyframes, {total_frames - key_count} delta frames.")
    print(f"Raw frame size: {frame_size} bytes/frame x {total_frames} = "
          f"{frame_size * total_frames:,} bytes uncompressed")
    print(f"Output file size: {out_size:,} bytes "
          f"({out_size / (frame_size * total_frames) * 100:.1f}% of raw)")


if __name__ == "__main__":
    main()