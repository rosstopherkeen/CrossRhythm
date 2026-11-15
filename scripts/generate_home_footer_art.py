#!/usr/bin/env python3
"""Generate the dashboard home footer art: streak digits and reader-type icons.

Sources live in assets/home-footer/:
  - BricolageGrotesque-wght-latin.woff2 (OFL, see OFL-BricolageGrotesque.txt),
    rendered at weight 800 for the streak digits 0-9.
  - dawn.svg, day.svg, dusk.svg, twilight.svg, new-reader.svg for the
    reader-type icons (Dawn, Day, Dusk, Twilight, New reader). Icons may use
    white fills inside black shapes; ink is whatever renders dark on paper.
    Each also yields a "Lit" variant (shapes as ink, plate dropped) for dark mode.

Sizing (agreed against the user's reference home screen):
  - Digits: the ink of "0" is DIGIT_HEIGHT px tall.
  - Icons: sized optically, so every icon has the same sqrt(inkW * inkH) of
    DIGIT_HEIGHT * ICON_OPTICAL. A square sun then reads as big as the tall cup.

Output: src/components/icons/homeFooterArt.h. Bitmaps use the GfxRenderer::drawIcon
layout (portrait framebuffer: stored row r is logical column width-1-r, bits run
down the logical column, MSB first, 1 = white).

Requires Pillow, cairosvg (libcairo), fontTools and brotli:
  python3 -m venv .venv && .venv/bin/pip install pillow cairosvg fonttools brotli
  .venv/bin/python scripts/generate_home_footer_art.py
"""

import io
import math
from pathlib import Path
import tempfile

import cairosvg
from fontTools.ttLib import TTFont
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "assets/home-footer"
OUT = ROOT / "src/components/icons/homeFooterArt.h"

DIGIT_HEIGHT = 42
DIGIT_WEIGHT = 800
ICON_OPTICAL = 1.25
THRESHOLD = 128
# Order matches ReadingTimeBucket (Morning, Afternoon, Evening, Night), then New reader.
ICONS = [
    ("Dawn", "dawn"),
    ("Day", "day"),
    ("Dusk", "dusk"),
    ("Twilight", "twilight"),
    ("NewReader", "new-reader"),
]


def ink_box(alpha):
    """Bounding box (x0, y0, x1, y1) exclusive of pixels at or above THRESHOLD."""
    mask = alpha.point(lambda v: 255 if v >= THRESHOLD else 0)
    box = mask.getbbox()
    if box is None:
        raise ValueError("no ink")
    return box


def pack(alpha):
    """Pack an alpha image (ink = high alpha) into drawIcon's rotated 1-bpp layout."""
    w, h = alpha.size
    px = alpha.load()
    stride = (h + 7) // 8
    data = []
    for r in range(w):
        lx = w - 1 - r
        for b in range(stride):
            byte = 0
            for bit in range(8):
                ly = b * 8 + bit
                white = 1
                if ly < h and px[lx, ly] >= THRESHOLD:
                    white = 0
                byte |= white << (7 - bit)
            data.append(byte)
    return data


def load_bricolage(tmpdir):
    font = TTFont(SRC / "BricolageGrotesque-wght-latin.woff2")
    font.flavor = None
    ttf = Path(tmpdir) / "bricolage.ttf"
    font.save(ttf)
    return ttf


def digit_font(ttf, size):
    font = ImageFont.truetype(str(ttf), size)
    font.set_variation_by_axes([DIGIT_WEIGHT])
    return font


def render_digit(font, ch):
    """Returns (alpha crop, xOffset from pen, rows of ink below baseline, advance)."""
    ascent, descent = font.getmetrics()
    pad = font.size
    img = Image.new("L", (font.size * 3, ascent + descent + pad * 2), 0)
    pen_x, baseline = pad, pad + ascent
    ImageDraw.Draw(img).text((pen_x, baseline), ch, font=font, fill=255, anchor="ls")
    x0, y0, x1, y1 = ink_box(img)
    return img.crop((x0, y0, x1, y1)), x0 - pen_x, y1 - baseline, round(font.getlength(ch))


def digit_size(ttf):
    """Font px size whose "0" ink is DIGIT_HEIGHT tall (same rule as the preview)."""
    zero = render_digit(digit_font(ttf, 100), "0")[0]
    return round(DIGIT_HEIGHT / (zero.size[1] / 100))


def ink_image(svg, scale):
    """Renders the SVG on paper and returns its darkness (255 = black ink), so white
    fills inside black shapes stay paper instead of counting as ink."""
    rgba = Image.open(io.BytesIO(cairosvg.svg2png(bytestring=svg, scale=scale))).convert("RGBA")
    paper = Image.new("RGBA", rgba.size, (255, 255, 255, 255))
    paper.alpha_composite(rgba)
    return paper.convert("L").point(lambda v: 255 - v)


def render_icon(name):
    svg = (SRC / f"{name}.svg").read_bytes()
    # Probe at ~2000 px on the long side, whatever the SVG's own units, to measure the ink.
    natural = Image.open(io.BytesIO(cairosvg.svg2png(bytestring=svg))).size
    probe_scale = 2000 / max(natural)
    x0, y0, x1, y1 = ink_box(ink_image(svg, probe_scale))
    ink_w, ink_h = (x1 - x0) / probe_scale, (y1 - y0) / probe_scale
    k = DIGIT_HEIGHT * ICON_OPTICAL / math.sqrt(ink_w * ink_h)
    img = ink_image(svg, k)
    return img.crop(ink_box(img)), render_lit_icon(svg, k)


LIT_SUPERSAMPLE = 4
LIT_PLATE_OPEN = 0.4  # opening size as a fraction of the icon's short side
LIT_PLATE_MARGIN = 3  # supersample px of slack for the plate's hand-drawn edge


def odd(n):
    return n | 1


def render_lit_icon(svg, k):
    """The icon as it should look on a dark background: the pictured shapes (not the
    black plate behind them) as ink, so the inverted draw lights them up.

    Shape = enclosed paper holes in the plate, plus the outline ring where the art
    overhangs the plate. The plate box is found on the filled silhouette (plate plus
    overhangs, holes closed): a morphological opening wider than any overhang leaves
    only the plate, and ink outside its box is the overhang outline.
    """
    ink = ink_image(svg, k * LIT_SUPERSAMPLE).point(lambda v: 255 if v >= THRESHOLD else 0)
    pad = 2
    padded = Image.new("L", (ink.width + 2 * pad, ink.height + 2 * pad), 0)
    padded.paste(ink, (pad, pad))
    paper = padded.point(lambda v: 255 - v)
    ImageDraw.floodfill(paper, (0, 0), 128)  # paper reachable from outside
    holes = paper.point(lambda v: 255 if v == 255 else 0)
    silhouette = paper.point(lambda v: 0 if v == 128 else 255)
    size = odd(round(min(ink.size) * LIT_PLATE_OPEN))
    plate = silhouette.filter(ImageFilter.MinFilter(size)).filter(ImageFilter.MaxFilter(size))
    box = plate.getbbox()
    if box is None:
        raise ValueError("no plate")
    m = LIT_PLATE_MARGIN
    ring = padded.copy()
    ImageDraw.Draw(ring).rectangle((box[0] - m, box[1] - m, box[2] - 1 + m, box[3] - 1 + m), fill=0)
    shape = ImageChops.lighter(holes, ring).crop((pad, pad, pad + ink.width, pad + ink.height))
    small = shape.resize(
        (max(1, round(ink.width / LIT_SUPERSAMPLE)), max(1, round(ink.height / LIT_SUPERSAMPLE))), Image.BOX
    )
    return small.crop(ink_box(small))


def c_array(name, data):
    lines = []
    for i in range(0, len(data), 18):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i : i + 18]) + ",")
    return f"static const uint8_t {name}[] = {{\n" + "\n".join(lines) + "\n};"


def main():
    with tempfile.TemporaryDirectory() as tmp:
        ttf = load_bricolage(tmp)
        size = digit_size(ttf)
        font = digit_font(ttf, size)
        digits = [render_digit(font, str(d)) for d in range(10)]

    rendered = [(ident, render_icon(name)) for ident, name in ICONS]
    icons = [(ident, img) for ident, (img, _) in rendered]
    lit_icons = [(ident + "Lit", lit) for ident, (_, lit) in rendered]

    out = [
        "#pragma once",
        "// Generated by scripts/generate_home_footer_art.py from assets/home-footer/. Do not edit.",
        f"// Digits: Bricolage Grotesque {DIGIT_WEIGHT} at {size}px (\"0\" ink {DIGIT_HEIGHT}px tall).",
        f"// Icons: optical size sqrt(w*h) = {DIGIT_HEIGHT * ICON_OPTICAL:g}px.",
        "// Bitmaps use the GfxRenderer::drawIcon layout.",
        "// *LitIcon: the pictured shapes alone (no plate), for the inverted/dark home.",
        "#include <cstdint>",
        "",
        "struct HomeFooterGlyph {",
        "  const uint8_t* bits;",
        "  uint8_t width;",
        "  uint8_t height;",
        "  int8_t xOffset;  // ink left edge relative to the pen position",
        "  int8_t below;    // ink rows below the baseline (round digits overshoot)",
        "  uint8_t advance; // pen advance to the next digit",
        "};",
        "",
        "struct HomeFooterIcon {",
        "  const uint8_t* bits;",
        "  uint8_t width;",
        "  uint8_t height;",
        "};",
        "",
    ]
    for d, (img, xoff, below, adv) in enumerate(digits):
        out.append(c_array(f"HomeFooterDigit{d}Bits", pack(img)))
    out.append("")
    out.append("static const HomeFooterGlyph HomeFooterDigits[10] = {")
    for d, (img, xoff, below, adv) in enumerate(digits):
        out.append(f"    {{HomeFooterDigit{d}Bits, {img.size[0]}, {img.size[1]}, {xoff}, {below}, {adv}}},")
    out.append("};")
    out.append("")
    for ident, img in icons + lit_icons:
        out.append(c_array(f"HomeFooter{ident}IconBits", pack(img)))
    out.append("")
    for ident, img in icons + lit_icons:
        out.append(f"static const HomeFooterIcon HomeFooter{ident}Icon = {{HomeFooter{ident}IconBits, {img.size[0]}, {img.size[1]}}};")
    out.append("")
    OUT.write_text("\n".join(out))
    print(f"wrote {OUT.relative_to(ROOT)}: digits {size}px, " + ", ".join(f"{i} {im.size[0]}x{im.size[1]}" for i, im in icons))


if __name__ == "__main__":
    main()
