#!/usr/bin/env python3
"""Generate the Inno Setup installer branding assets from the Pixelion logo.

Source : resources/pixelion-logo.png
Outputs: .github/scripts/assets/
  pixelion.ico              multi-res icon for SetupIconFile / uninstall entry
  wizard-large.bmp          164x314 (24-bit) left welcome/finish panel
  wizard-large-2x.bmp       328x628 hi-DPI variant
  wizard-large-3x.bmp       492x942 hi-DPI variant
  wizard-small.bmp          55x58   top-right inner-page logo
  wizard-small-2x.bmp       110x116 hi-DPI variant
  wizard-small-3x.bmp       165x174 hi-DPI variant

Inno Setup only accepts BMP for the wizard images and ICO for the setup icon.
BMPs are written as 24-bit (no alpha) because older Inno renderers mishandle
32-bit BMP alpha; the logo is composited onto a solid background instead.

Re-run this (needs Pillow: `pip install pillow`) whenever the logo changes,
then commit the regenerated files in .github/scripts/assets/.
"""
import os

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
LOGO = os.path.join(REPO, "resources", "pixelion-logo.png")
OUT = os.path.join(HERE, "assets")
os.makedirs(OUT, exist_ok=True)

# Pixelion brand: deep indigo panel so the colorful logo pops.
PANEL_BG = (26, 22, 46)     # near-black indigo
SMALL_BG = (255, 255, 255)  # inner pages are light; keep small logo on white

logo = Image.open(LOGO).convert("RGBA")


def flatten(img_rgba, bg):
    base = Image.new("RGB", img_rgba.size, bg)
    base.paste(img_rgba, mask=img_rgba.split()[-1])
    return base


def fit(img, box):
    """Return a copy of img scaled to fit inside box (w, h), preserving aspect."""
    c = img.copy()
    c.thumbnail(box, Image.LANCZOS)
    return c


def make_large(w, h, path):
    panel = Image.new("RGBA", (w, h), PANEL_BG + (255,))
    # Logo occupies ~70% of panel width, centered vertically a bit high.
    target = int(w * 0.70)
    scaled = fit(logo, (target, target))
    x = (w - scaled.width) // 2
    y = int(h * 0.30) - scaled.height // 2
    if y < int(h * 0.06):
        y = int(h * 0.06)
    panel.alpha_composite(scaled, (x, y))
    flatten(panel, PANEL_BG).save(path, "BMP")
    print("wrote", os.path.relpath(path, REPO))


def make_small(w, h, path):
    canvas = Image.new("RGBA", (w, h), SMALL_BG + (255,))
    pad = max(2, int(min(w, h) * 0.08))
    scaled = fit(logo, (w - 2 * pad, h - 2 * pad))
    x = (w - scaled.width) // 2
    y = (h - scaled.height) // 2
    canvas.alpha_composite(scaled, (x, y))
    flatten(canvas, SMALL_BG).save(path, "BMP")
    print("wrote", os.path.relpath(path, REPO))


# ICO: square logo flattened onto white, multi-resolution.
ico_src = flatten(logo, (255, 255, 255)).resize((256, 256), Image.LANCZOS)
ico_sizes = [(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
ico_path = os.path.join(OUT, "pixelion.ico")
ico_src.save(ico_path, format="ICO", sizes=ico_sizes)
print("wrote", os.path.relpath(ico_path, REPO))

make_large(164, 314, os.path.join(OUT, "wizard-large.bmp"))
make_large(328, 628, os.path.join(OUT, "wizard-large-2x.bmp"))
make_large(492, 942, os.path.join(OUT, "wizard-large-3x.bmp"))

make_small(55, 58, os.path.join(OUT, "wizard-small.bmp"))
make_small(110, 116, os.path.join(OUT, "wizard-small-2x.bmp"))
make_small(165, 174, os.path.join(OUT, "wizard-small-3x.bmp"))

print("done")
