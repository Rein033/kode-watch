#!/usr/bin/env python3
"""Kody for the watch: kodeOS's Lottie scenes rendered ahead of time.

The Dot plays Kody live (ThorVG, vector). The watch has no time for that, so
the scenes are rendered here, on black, at a lower rate, and packed as
run-length RGB565 frames in one blob for the "kody" data partition.

    kodyframes.py <KodeOS> <out.bin> [--png DIR] [--fps N]

Blob:  "KODZ" u16 nscenes, then per scene: name[16] u16 nframes u16 fps u32 first_frame_index
       u32 nframes_total, u32 offsets[nframes_total + 1] (from blob start)
       frames: rows of runs; each row: u16 nruns, then per run u16 count, u16 rgb565 (count 0 never);
       nruns 0xFFFF: the row is as in the frame before (the first frame of a scene has every row).
       Each frame is stored as one zlib stream (the ROM's tinfl inflates it on the watch).
Frame size is 410 x 334, drawn at the face's scene box.
"""
import argparse
import re
import struct
import zlib
from pathlib import Path

from PIL import Image
from rlottie_python import LottieAnimation

W, H = 410, 334
# Worlds of six scenes each, in kody_scene_t's order: loop, notice, plugged,
# looks, time, wake. The first scene of a world carries the world's name.
ROLES = ["{loop}", "{w}_lee_aviso", "{w}_enchufado", "{w}_te_mira", "{w}_mira_hora", "despierta_{w}"]


def world_scenes(w):
    loop = "reloj" if w == "reloj" else f"escena_{w}"
    return [r.format(w=w, loop=loop) for r in ROLES]


def lottie_json(kos: Path, name: str) -> str:
    js = kos / f"sdk/components/kui/kody/assets/kody_{name}.json"
    if js.exists():
        return js.read_text()
    src = (kos / f"sdk/components/kui/kody/src/kody_{name}.c").read_text()
    body = src[src.index("{", src.index("s_zlib[")) + 1:]
    body = body[:body.index("};")]
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))
    return zlib.decompress(data).decode()


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def encode(im: Image.Image, prev) -> bytes:
    px = im.load()
    out = bytearray()
    for y in range(H):
        if prev is not None and all(px[x, y] == prev[x, y] for x in range(W)):
            out += struct.pack("<H", 0xFFFF)
            continue
        runs, cur, n = [], None, 0
        for x in range(W):
            r, g, b = px[x, y]
            c = rgb565(r, g, b)
            if c == cur and n < 65535:
                n += 1
            else:
                if cur is not None:
                    runs.append((n, cur))
                cur, n = c, 1
        runs.append((n, cur))
        out += struct.pack("<H", len(runs))
        for cnt, col in runs:
            out += struct.pack("<HH", cnt, col)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kos", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--png", type=Path)
    ap.add_argument("--fps", type=int, default=15)
    ap.add_argument("--worlds", default="reloj")
    a = ap.parse_args()

    frames, table = [], []
    order = [(w, n) for w in a.worlds.split(",") for n in world_scenes(w)]
    for wi, (world, name) in enumerate(order):
        anim = LottieAnimation.from_data(lottie_json(a.kos, name))
        total = anim.lottie_animation_get_totalframe()
        src_fps = anim.lottie_animation_get_framerate()
        step = max(1, round(src_fps / a.fps))
        first = len(frames)
        prev = None
        for f in range(0, total, step):
            im = anim.render_pillow_frame(frame_num=f, width=W, height=H)
            bg = Image.new("RGB", (W, H), (0, 0, 0))
            bg.paste(im, mask=im.split()[3])
            frames.append(zlib.compress(encode(bg, prev), 9))
            prev = bg.load()
            if a.png and f == 0:
                a.png.mkdir(parents=True, exist_ok=True)
                bg.save(a.png / f"{name}.png")
        label = world if (wi % 6) == 0 else name
        table.append((label, len(frames) - first, round(src_fps / step), first))
        print(f"{name}: {total} frames at {src_fps} fps -> {len(frames) - first} at {round(src_fps / step)}")

    head = b"KODZ" + struct.pack("<H", len(table))
    for name, n, fps, first in table:
        head += name.encode()[:16].ljust(16, b"\0") + struct.pack("<HHI", n, fps, first)
    head += struct.pack("<I", len(frames))
    base = len(head) + 4 * (len(frames) + 1)
    offs, pos = [], base
    for fr in frames:
        offs.append(pos)
        pos += len(fr)
    offs.append(pos)
    blob = head + struct.pack(f"<{len(offs)}I", *offs) + b"".join(frames)
    a.out.write_bytes(blob)
    print(f"{len(frames)} frames, {len(blob)} bytes")


if __name__ == "__main__":
    main()
