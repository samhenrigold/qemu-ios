#!/usr/bin/env python3
"""art.py APP -- the kitchen sink's images, drawn here (no Apple artwork is copied): the home-screen icon,
the root list's row icons, three-part button backgrounds, the star, the button bar's tab icons and the big
image the Scroller screen pans over. Needs Pillow."""
import math, os, sys
from PIL import Image, ImageDraw, ImageFont

APP = sys.argv[1]
FONT = "/System/Library/Fonts/Helvetica.ttc"


def font(size, bold=False):
    try:
        return ImageFont.truetype(FONT, size, index=1 if bold else 0)
    except OSError:
        return ImageFont.load_default()


def save(img, name):
    img.save(os.path.join(APP, name))


def gradient(w, h, top, bottom):
    img = Image.new("RGBA", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    return img


def rounded_mask(w, h, r):
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, w - 1, h - 1], r, fill=255)
    return m


# Home-screen icon: a faucet over a basin (SpringBoard adds the gloss and corners).
icon = gradient(57, 57, (90, 160, 220), (30, 70, 140))
d = ImageDraw.Draw(icon)
d.rounded_rectangle([8, 30, 48, 47], 6, fill=(235, 240, 248))
d.rectangle([12, 30, 44, 35], fill=(190, 200, 215))
d.rectangle([26, 12, 31, 30], fill=(220, 225, 235))
d.rectangle([26, 12, 42, 17], fill=(220, 225, 235))
d.ellipse([38, 18, 44, 24], fill=(120, 200, 255))
save(icon.convert("RGB"), "icon.png")

# Root list row icons: a rounded tile per screen.
tiles = [(52, 120, 220), (60, 170, 90), (230, 150, 40), (210, 60, 60), (120, 90, 200), (40, 160, 170),
         (90, 110, 130), (130, 130, 140), (200, 90, 150), (240, 120, 60), (70, 140, 220), (50, 60, 80),
         (160, 120, 60), (60, 130, 200), (150, 160, 60)]
glyphs = ["B", "S", "T", "!", "%", "P", "I", "☰", "A", "⇄", "↻", "N", "★", "W", "L"]
for i, (c, g) in enumerate(zip(tiles, glyphs)):
    t = gradient(29, 29, tuple(min(255, v + 50) for v in c), c)
    t.putalpha(rounded_mask(29, 29, 6))
    d = ImageDraw.Draw(t)
    d.text((14.5, 15), g, font=font(17, True), fill=(255, 255, 255), anchor="mm")
    save(t, "Row%d.png" % i)

# Three-part button backgrounds, 30x44: 14 left, 2 middle, 14 right.
for name, top, bottom in (("Green", (110, 200, 100), (40, 140, 50)), ("Red", (235, 100, 90), (180, 30, 30)),
                          ("Gray", (150, 160, 175), (85, 95, 110))):
    for pressed in (False, True):
        a, b = (bottom, top) if pressed else (top, bottom)
        img = gradient(30, 44, a, b)
        shine = Image.new("RGBA", (30, 22), (255, 255, 255, 50 if not pressed else 20))
        img.alpha_composite(shine, (0, 0))
        img.putalpha(rounded_mask(30, 44, 9))
        ImageDraw.Draw(img).rounded_rectangle([0, 0, 29, 43], 9, outline=(0, 0, 0, 90))
        save(img, "Button-%s%s.png" % (name, "-Pressed" if pressed else ""))


def star(fill, outline):
    img = Image.new("RGBA", (28, 28), (0, 0, 0, 0))
    pts = [(14 + 13 * math.sin(i * math.pi / 5) * (1 if i % 2 == 0 else 0.45),
            14 - 13 * math.cos(i * math.pi / 5) * (1 if i % 2 == 0 else 0.45)) for i in range(10)]
    ImageDraw.Draw(img).polygon(pts, fill=fill, outline=outline)
    return img


save(star((0, 0, 0, 0), (40, 90, 180, 255)), "Star.png")
save(star((250, 190, 40, 255), (180, 120, 0, 255)), "StarOn.png")


# Button bar tab icons (30x30): gray and the selected blue.
def tab(i, color):
    img = Image.new("RGBA", (30, 30), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    if i == 0:
        img = star(color, color).resize((28, 28))
        out = Image.new("RGBA", (30, 30), (0, 0, 0, 0))
        out.alpha_composite(img, (1, 1))
        return out
    if i == 1:
        d.ellipse([3, 3, 27, 27], outline=color, width=3)
        d.line([15, 15, 15, 7], fill=color, width=3)
        d.line([15, 15, 21, 18], fill=color, width=3)
    elif i == 2:
        d.ellipse([9, 3, 21, 15], fill=color)
        d.pieslice([3, 14, 27, 38], 180, 360, fill=color)
    elif i == 3:
        for r in range(3):
            for c in range(3):
                d.ellipse([3 + c * 9, 3 + r * 9, 9 + c * 9, 9 + r * 9], fill=color)
    else:
        d.ellipse([2, 9, 14, 21], outline=color, width=3)
        d.ellipse([16, 9, 28, 21], outline=color, width=3)
        d.line([8, 21, 22, 21], fill=color, width=3)
    return img


for i in range(5):
    save(tab(i, (150, 150, 150, 255)), "Tab%d.png" % i)
    save(tab(i, (90, 170, 255, 255)), "Tab%dSelected.png" % i)

# The Scroller screen's image: a 720x1000 chart of an imaginary archipelago with a grid and labels.
W, H = 720, 1000
m = Image.new("RGB", (W, H), (40, 90, 150))
d = ImageDraw.Draw(m)
for x in range(0, W, 60):
    d.line([x, 0, x, H], fill=(60, 110, 170))
for y in range(0, H, 60):
    d.line([0, y, W, y], fill=(60, 110, 170))
islands = [(180, 200, 120, "Cupertino Key"), (520, 300, 90, "Infinite Isle"), (300, 560, 150, "Darwin"),
           (560, 760, 100, "Kernel Rock"), (140, 840, 80, "Springboard")]
for cx, cy, r, name in islands:
    pts = [(cx + r * (0.75 + 0.25 * math.sin(k * 1.7)) * math.cos(k * math.pi / 12),
            cy + r * (0.75 + 0.25 * math.cos(k * 2.3)) * math.sin(k * math.pi / 12)) for k in range(24)]
    d.polygon(pts, fill=(230, 210, 150))
    inner = [(cx + (x - cx) * 0.7, cy + (y - cy) * 0.7) for x, y in pts]
    d.polygon(inner, fill=(110, 170, 90))
    d.text((cx, cy), name, font=font(18, True), fill=(30, 40, 30), anchor="mm")
for i, ch in enumerate("ABCDEFGHIJKL"):
    d.text((30 + i * 60, 12), ch, font=font(14, True), fill=(200, 220, 240), anchor="mm")
for j in range(1, 17):
    d.text((10, 30 + j * 60), str(j), font=font(14, True), fill=(200, 220, 240), anchor="lm")
d.text((W / 2, H - 30), "Kitchen Sink, 2007 - drag me", font=font(20, True), fill=(255, 255, 255), anchor="mm")
save(m, "Map.png")
