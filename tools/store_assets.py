#!/usr/bin/env python3
"""Build the appstore images in store/ from emulator screenshots.

    python3 tools/store_assets.py RAW_DIR

RAW_DIR holds emulator screenshots named <platform>_<shot>.png: gif_-32 ...
gif_0 (E2 tuned up into the green), intune, sharp, accidental, settings.
Writes store/screenshots/<platform>_<n>_<shot>.{gif,png} (the appstore reads
the platform from the file name prefix), store/icon_small.png (80x80),
store/icon_large.png (144x144) and store/banner_<platform>.png (720x320).
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORE = os.path.join(ROOT, "store")
FONTS = os.path.join(ROOT, "resources", "fonts")
PLATFORMS = ["emery", "gabbro"]
GIF_CENTS = [-32, -21, -12, -6, -3, -1, 0]

# The app's palette (src/c/theme.h) as the emulator's screenshots render it,
# i.e. as the watch's display shows it, so banners blend with screenshots.
NAVY = (0, 30, 65)
INACTIVE = (79, 103, 144)
GREEN = (142, 230, 158)
GREY = (171, 171, 171)


def tuning_gif(raw, platform, out):
    frames = [Image.open(os.path.join(raw, f"{platform}_gif_{c}.png")).convert("RGB")
              for c in GIF_CENTS]
    # One exact palette for all frames: the watch only has 64 colours.
    colours = sorted({c for f in frames for _, c in f.getcolors(1 << 16)})
    palette = Image.new("P", (1, 1))
    flat = [v for c in colours for v in c]
    palette.putpalette(flat + [0] * (768 - len(flat)))
    frames = [f.quantize(palette=palette, dither=Image.Dither.NONE) for f in frames]
    durations = [700] * (len(frames) - 1) + [2200]
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=durations, loop=0)


def icon(size):
    s = 8
    img = Image.new("RGBA", (size * s, size * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, size * s - 1, size * s - 1), radius=int(size * s * 0.22),
                        fill=NAVY + (255,))
    # The tuner's deviation bars, centre lit.
    heights = [0.20, 0.34, 0.34, 0.60, 0.34, 0.34, 0.20]
    pitch = size * s * 0.105
    width = size * s * 0.052
    cx = size * s / 2
    cy = size * s / 2
    for i, h in enumerate(heights):
        x = cx + (i - 3) * pitch
        half = h * size * s / 2
        colour = GREEN if i == 3 else INACTIVE
        w = width * (1.35 if i == 3 else 1.0)
        d.rounded_rectangle((x - w / 2, cy - half, x + w / 2, cy + half), radius=w / 2,
                            fill=colour + (255,))
    return img.resize((size, size), Image.LANCZOS)


def banner(raw, platform, out):
    w, h = 720, 320
    img = Image.new("RGB", (w, h), NAVY)
    shot = Image.open(os.path.join(raw, f"{platform}_intune.png")).convert("RGB")
    if platform == "gabbro":
        mask = Image.new("L", shot.size, 0)
        ImageDraw.Draw(mask).ellipse((0, 0, shot.width - 1, shot.height - 1), fill=255)
        x = w - shot.width - 40
        img.paste(shot, (x, (h - shot.height) // 2), mask)
    else:
        x = w - shot.width - 60
        img.paste(shot, (x, (h - shot.height) // 2))
    d = ImageDraw.Draw(img)
    title = ImageFont.truetype(os.path.join(FONTS, "NotoSans-Bold.ttf"), 58)
    body = ImageFont.truetype(os.path.join(FONTS, "NotoSans-Regular.ttf"), 26)
    left = 44
    d.text((left, 92), "Guitar Tuner", font=title, fill=(255, 255, 255))
    d.text((left, 176), "A chromatic tuner that", font=body, fill=GREY)
    d.text((left, 210), "listens on your wrist", font=body, fill=GREY)
    img.save(out, optimize=True)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    raw = sys.argv[1]
    shots = os.path.join(STORE, "screenshots")
    os.makedirs(shots, exist_ok=True)
    for platform in PLATFORMS:
        tuning_gif(raw, platform, os.path.join(shots, f"{platform}_1_tuning.gif"))
        for n, name in enumerate(["intune", "sharp", "accidental", "settings"], start=2):
            Image.open(os.path.join(raw, f"{platform}_{name}.png")).convert("RGB").save(
                os.path.join(shots, f"{platform}_{n}_{name}.png"), optimize=True)
        banner(raw, platform, os.path.join(STORE, f"banner_{platform}.png"))
    icon(80).save(os.path.join(STORE, "icon_small.png"), optimize=True)
    icon(144).save(os.path.join(STORE, "icon_large.png"), optimize=True)


if __name__ == "__main__":
    main()
