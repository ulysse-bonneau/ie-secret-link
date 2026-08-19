#!/usr/bin/env python3
"""Generate 3DS home-menu assets: icon.png (48x48), banner.png (256x128),
audio.wav (banner jingle), and optionally a QR code for a download URL.

Usage: gen_assets.py [--qr URL out.png]
Needs pillow (and qrcode for --qr).
"""

import math
import struct
import sys
from PIL import Image, ImageDraw, ImageFont

ASSETS = __file__.rsplit("/", 2)[0] + "/3ds/assets"

NAVY = (12, 16, 48)
PURPLE = (44, 20, 84)
BOLT = (255, 200, 30)
BOLT_HI = (255, 240, 140)


def space_bg(w, h, seed=7):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / h
        base = tuple(int(a + (b - a) * t) for a, b in zip(NAVY, PURPLE))
        for x in range(w):
            px[x, y] = base
    rng = (seed * 2654435761) & 0xFFFFFFFF
    for _ in range(w * h // 90):
        rng = (rng * 1103515245 + 12345) & 0x7FFFFFFF
        x, y = rng % w, (rng >> 12) % h
        b = 140 + (rng >> 22) % 116
        px[x, y] = (b, b, min(255, b + 30))
    return img


def draw_bolt(d, cx, top, bot, w):
    """Zigzag lightning bolt centered on cx, from top to bot."""
    mid1 = top + (bot - top) * 0.45
    mid2 = top + (bot - top) * 0.55
    pts = [
        (cx + w * 0.45, top), (cx - w * 0.35, mid1), (cx + w * 0.05, mid1),
        (cx - w * 0.45, bot), (cx + w * 0.35, mid2), (cx - w * 0.05, mid2),
    ]
    d.polygon(pts, fill=BOLT, outline=BOLT_HI)


def draw_ball(d, cx, cy, r):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(245, 245, 250), outline=(30, 30, 40))
    p = r * 0.42
    pent = [(cx + p * math.sin(a), cy - p * math.cos(a))
            for a in (i * 2 * math.pi / 5 for i in range(5))]
    d.polygon(pent, fill=(25, 25, 35))
    for a in (i * 2 * math.pi / 5 - math.pi / 5 for i in range(5)):
        x1 = cx + p * 1.05 * math.sin(a); y1 = cy - p * 1.05 * math.cos(a)
        x2 = cx + r * 0.98 * math.sin(a); y2 = cy - r * 0.98 * math.cos(a)
        d.line([x1, y1, x2, y2], fill=(25, 25, 35), width=max(1, r // 12))


def make_icon():
    s = 4  # supersample
    img = space_bg(48 * s, 48 * s)
    d = ImageDraw.Draw(img)
    draw_ball(d, 24 * s, 26 * s, 16 * s)
    draw_bolt(d, 24 * s, 2 * s, 46 * s, 20 * s)
    img.resize((48, 48), Image.LANCZOS).save(ASSETS + "/icon.png")


def big_text(text, scale):
    """Chunky pixel text from PIL's built-in bitmap font."""
    f = ImageFont.load_default()
    l, t, r, b = f.getbbox(text)
    im = Image.new("RGBA", (r - l + 2, b - t + 2), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((1 - l, 1 - t), text, font=f, fill=(255, 255, 255, 255))
    return im.resize((im.width * scale, im.height * scale), Image.NEAREST)


def stamp(img, txt_img, cx, cy, fill, outline=(10, 10, 25)):
    solid = Image.new("RGBA", txt_img.size, outline + (255,))
    for dx in (-2, 0, 2):
        for dy in (-2, 0, 2):
            img.paste(solid, (cx - txt_img.width // 2 + dx, cy - txt_img.height // 2 + dy), txt_img)
    solid = Image.new("RGBA", txt_img.size, fill + (255,))
    img.paste(solid, (cx - txt_img.width // 2, cy - txt_img.height // 2), txt_img)


def make_banner():
    s = 2
    img = space_bg(256 * s, 128 * s, seed=13).convert("RGBA")
    d = ImageDraw.Draw(img)
    draw_ball(d, 218 * s, 74 * s, 34 * s)
    draw_bolt(d, 218 * s, 18 * s, 122 * s, 30 * s)
    img = img.resize((256, 128), Image.LANCZOS)
    stamp(img, big_text("IESM", 4), 100, 48, BOLT)
    stamp(img, big_text("Inazuma Eleven", 1), 100, 82, (230, 230, 240))
    stamp(img, big_text("Save Manager", 1), 100, 96, (230, 230, 240))
    img.convert("RGB").save(ASSETS + "/banner.png")


def make_audio():
    sr = 22050
    notes = [(660, 0.12), (880, 0.12), (1320, 0.30)]  # E5 A5 E6 chime
    samples = []
    for freq, dur in notes:
        n = int(sr * dur)
        for i in range(n):
            env = min(1.0, i / (sr * 0.01)) * (1 - i / n) ** 1.5
            v = 0.5 * env * (math.sin(2 * math.pi * freq * i / sr)
                             + 0.3 * math.sin(4 * math.pi * freq * i / sr))
            samples.append(int(v * 32767))
    data = b"".join(struct.pack("<h", v) for v in samples)
    hdr = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt "
           + struct.pack("<IHHIIHH", 16, 1, 1, sr, sr * 2, 2, 16)
           + b"data" + struct.pack("<I", len(data)))
    open(ASSETS + "/audio.wav", "wb").write(hdr + data)


def make_qr(url, out):
    import qrcode
    qrcode.make(url).save(out)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--qr":
        make_qr(sys.argv[2], sys.argv[3])
    else:
        make_icon()
        make_banner()
        make_audio()
    print("assets written")
