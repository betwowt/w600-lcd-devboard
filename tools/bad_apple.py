#!/usr/bin/env python3
"""Convert a video to the W600 Bad Apple 1-bpp frame stream.

Output layout (little-endian, 32-byte header):

    0   "BADAPPL1"
    8   u16 width        (multiple of 8, <= 240)
   10   u16 height       (<= 320)
   12   u16 x            screen x offset
   14   u16 y            screen y offset
    16   u16 fps
   18   u16 flags        (0 = raw, 1 = length-prefixed RLE)
   20   u32 frame_count
   24   u32 frame_bytes  ((width / 8) * height, uncompressed)
   28   u32 reserved

Every frame is row-major, 1 bit per pixel, MSB first, 1 = white.  By
default each frame is stored as a u16 payload length followed by a LEB128
run-length stream (bit 15 of the length = raw fallback); --raw writes the
old fixed-size unpacked stream instead.  The firmware expands it to RGB565
and streams it to the ILI9341.

Requires ffmpeg and numpy:

    python tools/bad_apple.py bad_apple.mp4 -o badapple.bin
    python tools/bad_apple.py bad_apple.mp4 -o badapple.bin --fps 15 --dither none
"""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys

import numpy as np

MAGIC = b"BADAPPL1"
HEADER = struct.Struct("<8sHHHHHHIII")

BAYER8 = np.array([
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
], dtype=np.float32)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("input", help="video/GIF/image sequence understood by ffmpeg")
    p.add_argument("-o", "--output", default="badapple.bin", help="output file")
    p.add_argument("--fps", type=int, default=12, help="playback frame rate (integer)")
    p.add_argument("--width", type=int, default=240, help="frame width (multiple of 8)")
    p.add_argument("--height", type=int, default=180, help="frame height")
    p.add_argument("--x", type=int, default=None, help="screen x (default: centered)")
    p.add_argument("--y", type=int, default=None, help="screen y (default: centered)")
    p.add_argument("--start", type=float, default=0.0, help="start time, seconds")
    p.add_argument("--duration", type=float, default=None, help="max duration, seconds")
    p.add_argument("--max-frames", type=int, default=None, help="stop after N frames")
    p.add_argument("--threshold", type=int, default=128,
                   help="black/white threshold for --dither none (0-255)")
    p.add_argument("--dither", choices=("ordered", "none"), default="none",
                   help="hard threshold (default, best RLE) or 8x8 Bayer dither")
    p.add_argument("--invert", action="store_true", help="swap black and white")
    p.add_argument("--raw", action="store_true",
                   help="write fixed-size raw 1-bpp frames (old firmware format)")
    p.add_argument("--ffmpeg", default="ffmpeg", help="ffmpeg executable")
    return p.parse_args()


def check_args(a: argparse.Namespace) -> None:
    if a.width % 8:
        raise SystemExit("--width must be a multiple of 8 (1-bpp rows are byte aligned)")
    if not (8 <= a.width <= 240) or not (1 <= a.height <= 320):
        raise SystemExit("frame does not fit the 240x320 panel")
    if a.fps <= 0 or a.fps > 60:
        raise SystemExit("--fps must be in 1..60")
    if a.x is None:
        a.x = (240 - a.width) // 2
    if a.y is None:
        a.y = (320 - a.height) // 2
    if a.x < 0 or a.y < 0 or a.x + a.width > 240 or a.y + a.height > 320:
        raise SystemExit("frame plus offset does not fit the 240x320 panel")
    if not shutil.which(a.ffmpeg):
        raise SystemExit(f"ffmpeg not found: {a.ffmpeg}")


def ffmpeg_cmd(a: argparse.Namespace) -> list[str]:
    vf = (
        f"fps={a.fps:g},"
        f"scale={a.width}:{a.height}:force_original_aspect_ratio=decrease:"
        f"flags=lanczos,"
        f"pad={a.width}:{a.height}:(ow-iw)/2:(oh-ih)/2:color=black,"
        f"format=gray"
    )
    cmd = [a.ffmpeg, "-v", "error", "-nostdin"]
    if a.start > 0:
        cmd += ["-ss", f"{a.start:g}"]
    cmd += ["-i", a.input]
    if a.duration is not None:
        cmd += ["-t", f"{a.duration:g}"]
    cmd += ["-an", "-sn", "-dn", "-vf", vf, "-f", "rawvideo", "-pix_fmt", "gray", "-"]
    return cmd


def binarize(gray: np.ndarray, a: argparse.Namespace) -> np.ndarray:
    if a.dither == "ordered":
        h, w = gray.shape
        tile = np.tile(BAYER8, (h // 8 + 1, w // 8 + 1))[:h, :w]
        bits = gray.astype(np.float32) > (tile + 0.5) * (255.0 / 64.0)
    else:
        bits = gray >= a.threshold
    if a.invert:
        bits = ~bits
    return bits.astype(np.uint8)


def encode_rle(bits: np.ndarray) -> bytes:
    """LEB128 run lengths, row-major, starting with black.

    A zero-length first run means the first pixel is white.  The decoder
    stops after width*height pixels, so the stream needs no run count.
    """
    flat = bits.ravel()
    idx = np.flatnonzero(flat[1:] != flat[:-1]) + 1
    lens = np.diff(np.concatenate(([0], idx, [flat.size])))
    if flat[0]:
        lens = np.concatenate(([0], lens))

    out = bytearray()
    for n in lens.tolist():
        while n >= 0x80:
            out.append((n & 0x7F) | 0x80)
            n >>= 7
        out.append(n)
    return bytes(out)


def main() -> int:
    a = parse_args()
    check_args(a)

    frame_bytes = (a.width // 8) * a.height
    stride_px = a.width * a.height
    out = open(a.output, "wb")
    out.write(b"\x00" * HEADER.size)

    cmd = ffmpeg_cmd(a)
    print("ffmpeg:", " ".join(cmd), file=sys.stderr)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=None)

    frames = 0
    written = 0
    try:
        while True:
            raw = proc.stdout.read(stride_px)
            if not raw:
                break
            if len(raw) != stride_px:
                print(f"warning: dropping partial frame ({len(raw)} bytes)", file=sys.stderr)
                break
            gray = np.frombuffer(raw, dtype=np.uint8).reshape(a.height, a.width)
            bits = binarize(gray, a)
            packed = np.packbits(bits, axis=1, bitorder="big")
            if a.raw:
                out.write(packed.tobytes())
                written += frame_bytes
            else:
                rle = encode_rle(bits)
                if len(rle) < frame_bytes:
                    out.write(struct.pack("<H", len(rle)))
                    out.write(rle)
                    written += 2 + len(rle)
                else:
                    out.write(struct.pack("<H", 0x8000 | frame_bytes))
                    out.write(packed.tobytes())
                    written += 2 + frame_bytes
            frames += 1
            if frames % 100 == 0:
                print(f"\r{frames} frames, {written / 1048576:.1f} MiB",
                      end="", file=sys.stderr, flush=True)
            if a.max_frames is not None and frames >= a.max_frames:
                proc.kill()
                break
    finally:
        out.seek(0)
        out.write(HEADER.pack(
            MAGIC, a.width, a.height, a.x, a.y,
            a.fps, 0 if a.raw else 1, frames, frame_bytes, 0
        ))
        out.close()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    if frames == 0:
        os.unlink(a.output)
        print("no frames decoded", file=sys.stderr)
        return 1

    total = HEADER.size + written
    avg = written / frames
    print(
        f"\n{frames} frames, {a.width}x{a.height} at ({a.x},{a.y}), "
        f"{a.fps:g} fps, {total / 1048576:.2f} MiB -> {a.output}",
        file=sys.stderr,
    )
    print(
        f"stream rate: {avg * a.fps / 1024:.1f} KiB/s "
        f"({avg:.0f} B/frame, {100 * avg / frame_bytes:.0f}% of raw); "
        f"playback duration {frames / a.fps:.1f} s",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
