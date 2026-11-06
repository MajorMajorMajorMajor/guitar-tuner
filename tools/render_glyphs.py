#!/usr/bin/env python3
"""Pre-render the big note names and accidentals as PNG resources.

Pebble font glyphs are limited to 4096 pixels and are 1-bit, which is too
small for a Chroma-sized note letter. Images have no
such limit and keep their anti-aliasing on colour watches, so the note names
are drawn here once, per watch size, with Pillow:

    python3 tools/render_glyphs.py

Writes resources/glyphs/<name>~<platform tag>.png (and the menu icon). The
outputs are committed; rerun this only after changing sizes or fonts.
"""

import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "resources", "fonts")
OUT = os.path.join(ROOT, "resources", "glyphs")

NOTE_FONT = os.path.join(FONTS, "NotoSans-Bold.ttf")
MUSIC_FONT = os.path.join(FONTS, "NotoMusic-Regular.ttf")

# Platform tag -> cap height of the note letters in px. Keep in sync with
# NOTE_CAP in src/c/ui.c.
TARGETS = {
    "emery": 60,  # Pebble Time 2: 200x228
    "gabbro": 64,  # Pebble Round 2: 260x260 round
}

NOTES = {
    "C": "C", "D": "D", "E": "E", "F": "F", "G": "G", "A": "A", "B": "B",
    "DO": "Do", "RE": "Re", "MI": "Mi", "FA": "Fa", "SOL": "Sol", "LA": "La", "SI": "Si",
}
ACCIDENTALS = {"SHARP": "♯", "FLAT": "♭"}
# Accidental height relative to the note cap height (Chroma: ~0.6).
ACCIDENTAL_SCALE = 0.62

SUPERSAMPLE = 4
CAP_RATIO = 0.72  # Noto Sans cap height / em
TOP_RATIO = 0.79  # tallest ascender ('l', 'S') above the baseline / em
BOTTOM_RATIO = 0.02  # overshoot of round letters below the baseline / em


def finish(mask):
    """Grey coverage mask (L) -> white glyph with 2-bit alpha, matching
    Pebble's ARGB2222 so nothing is lost or dithered in conversion."""
    alpha = mask.point(lambda v: min(3, (v + 42) // 85) * 85)
    img = Image.new("RGBA", mask.size, (255, 255, 255, 0))
    img.putalpha(alpha)
    return img


def render_text(text, font_path, size_px, top, bottom):
    """Coverage mask of `text`, cropped horizontally, rows top..bottom
    relative to the baseline (px, positive is up), supersampled."""
    s = SUPERSAMPLE
    font = ImageFont.truetype(font_path, size_px * s)
    ascent, descent = font.getmetrics()
    left, _, right, _ = font.getbbox(text)
    canvas = Image.new("L", (right - left + 4 * s, ascent + descent), 0)
    ImageDraw.Draw(canvas).text((-left + 2 * s, 0), text, font=font, fill=255)
    bbox = canvas.getbbox()
    x0, x1 = bbox[0], bbox[2]
    crop = canvas.crop((x0, ascent - top * s, x1, ascent + bottom * s))
    w = max(1, round(crop.width / s))
    return crop.resize((w, top + bottom), Image.LANCZOS)


def render_accidental(symbol, height):
    s = SUPERSAMPLE
    font = ImageFont.truetype(MUSIC_FONT, 200 * s)
    left, top, right, bottom = font.getbbox(symbol)
    scale = height / ((bottom - top) / s)
    font = ImageFont.truetype(MUSIC_FONT, round(200 * s * scale))
    left, top, right, bottom = font.getbbox(symbol)
    canvas = Image.new("L", (right - left + 4 * s, bottom - top + 4 * s), 0)
    ImageDraw.Draw(canvas).text((-left + 2 * s, -top + 2 * s), symbol, font=font, fill=255)
    # Noto Music's strokes are hairlines next to bold letters: thicken them by
    # about a pixel, like Chroma's chunkier accidentals.
    canvas = canvas.filter(ImageFilter.MaxFilter(2 * (s // 2) + 1 + 2))
    canvas = canvas.crop(canvas.getbbox())
    w = max(1, round(canvas.width / s))
    return canvas.resize((w, height), Image.LANCZOS)


def menu_icon():
    """25x25 icon: a little deviation meter with the centre bar lit. Black,
    like the system's own icons: the launcher draws it as-is on both its
    white rows and its coloured highlight, so white would vanish."""
    s = 8
    img = Image.new("L", (25 * s, 25 * s), 0)
    d = ImageDraw.Draw(img)
    heights = [8, 12, 22, 12, 8]
    for i, h in enumerate(heights):
        cx = (4 + i * 4.25) * s
        w = 2.2 * s if i == 2 else 1.6 * s
        d.rounded_rectangle(
            (cx - w, (12.5 - h / 2) * s, cx + w, (12.5 + h / 2) * s), radius=w, fill=255
        )
    mask = img.resize((25, 25), Image.LANCZOS)
    alpha = mask.point(lambda v: min(3, (v + 42) // 85) * 85)
    out = Image.new("RGBA", (25, 25), (0, 0, 0, 0))
    out.putalpha(alpha)
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    for tag, cap in TARGETS.items():
        size = cap / CAP_RATIO
        top = round(TOP_RATIO * size)
        bottom = max(1, round(BOTTOM_RATIO * size))
        for name, text in NOTES.items():
            img = finish(render_text(text, NOTE_FONT, round(size), top, bottom))
            img.save(os.path.join(OUT, f"note_{name.lower()}~{tag}.png"), optimize=True)
        acc_h = round(cap * ACCIDENTAL_SCALE)
        for name, symbol in ACCIDENTALS.items():
            img = finish(render_accidental(symbol, acc_h))
            img.save(os.path.join(OUT, f"acc_{name.lower()}~{tag}.png"), optimize=True)
        print(f"{tag}: note {cap}px cap, image height {top + bottom}px, accidental {acc_h}px")
    menu_icon().save(os.path.join(ROOT, "resources", "menu_icon.png"), optimize=True)


if __name__ == "__main__":
    main()
