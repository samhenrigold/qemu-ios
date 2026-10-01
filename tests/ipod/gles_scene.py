"""Judge GLTest's declared scene, including placement rather than color totals.

The fixture in contrib/it-gles/glapp.c draws into (40,60,240,360): cyan on
the left, magenta above yellow on the right. Interior regions avoid edge
rasterization differences. No golden frame or host-GPU-specific hash is used.
"""


def color(rgb, maximum):
    lo, up = maximum * 0.3, maximum * 0.7
    r, g, b = rgb
    if r >= up and b >= up and g <= lo:
        return "magenta"
    if g >= up and b >= up and r <= lo:
        return "cyan"
    if r >= up and g >= up and b <= lo:
        return "yellow"
    return None


def fractions(width, height, pixels):
    if width <= 0 or height <= 0 or len(pixels) != width * height * 3:
        raise ValueError("invalid RGB frame dimensions")
    maximum = max(pixels) or 1
    counts = dict.fromkeys(("magenta", "cyan", "yellow"), 0)
    for offset in range(0, len(pixels), 3):
        name = color(pixels[offset:offset + 3], maximum)
        if name:
            counts[name] += 1
    return tuple(counts[name] / (width * height) for name in counts)


def verdict(width, height, pixels):
    if (width, height) != (320, 480) or len(pixels) != width * height * 3:
        return False, "GLTest needs a 320x480 RGB panel frame"
    maximum = max(pixels) or 1
    regions = (("cyan", (44, 64, 156, 416)),
               ("magenta", (164, 64, 276, 236)),
               ("yellow", (164, 244, 276, 416)))
    for expected, (x0, y0, x1, y1) in regions:
        matched = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                offset = (y * width + x) * 3
                matched += color(pixels[offset:offset + 3], maximum) == expected
        purity = matched / ((x1 - x0) * (y1 - y0))
        if purity < 0.95:
            return False, "%s region has %.3f correct pixels; scene geometry/stride is wrong" % (expected, purity)
    outside = matched = 0
    for y in range(height):
        for x in range(width):
            if 40 <= x < 280 and 60 <= y < 420:
                continue
            outside += 1
            offset = (y * width + x) * 3
            matched += color(pixels[offset:offset + 3], maximum) is not None
    if matched / outside > 0.01:
        return False, "GL scene colors escape the declared inset view"
    return True, "solid scene regions occupy the declared inset view"
