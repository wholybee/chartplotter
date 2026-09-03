#!/usr/bin/env python3
"""Generate the HMV Chartplotter application icon.

Draws a marine compass rose on a deep-navy rounded tile and writes:

    resources/appicon.png    256x256 master (also used by the Qt window icon)
    resources/appicon.ico    multi-resolution Windows icon (16..256)

The .ico is what the Windows executable (via appicon.rc) and the NSIS
installer embed; the .png is embedded as a Qt resource so the running app
shows the icon in the taskbar/window on every platform.

Each frame is rendered independently at 4x supersampling and box-filtered
down, so small sizes stay crisp instead of being a blurred downscale of the
256px master. Frames up to 128px are stored as 32-bit BGRA BMPs (the most
widely compatible ICO layout, including older NSIS/Explorer paths); the 256px
frame is stored as PNG (the standard for that size).

Pure Pillow, no SVG rasteriser required.  Re-run after editing to regenerate:

    python tools/make_app_icon.py
"""

import io
import math
import os
import struct

from PIL import Image, ImageDraw

# ---- palette ---------------------------------------------------------------
BG_TOP      = (16, 60, 96)     # ocean navy, lit top
BG_BOT      = (6, 34, 58)      # ocean navy, deep bottom
RING        = (150, 194, 226)  # bearing ring / ticks, pale blue
STAR_LIGHT  = (245, 249, 252)  # compass point, lit facet
STAR_DARK   = (150, 173, 196)  # compass point, shaded facet
NORTH_LIGHT = (240, 92, 74)    # north point, lit facet (red)
NORTH_DARK  = (176, 42, 38)    # north point, shaded facet (deep red)
HUB_GOLD    = (247, 202, 88)   # centre hub
HUB_CORE    = (10, 42, 68)     # hub centre dot

SS = 4  # supersampling factor


def _lerp(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def _u(angle_deg):
    """Unit vector for a bearing measured clockwise from straight up."""
    r = math.radians(angle_deg)
    return (math.sin(r), -math.cos(r))


def render(size):
    """Render one RGBA frame at the requested pixel size."""
    S = size * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Rounded-tile background with a soft top->bottom ocean gradient. Draw the
    # gradient as horizontal bands, then punch it through a rounded-rect mask.
    grad = Image.new("RGB", (S, S))
    gd = ImageDraw.Draw(grad)
    for y in range(S):
        gd.line([(0, y), (S, y)], fill=_lerp(BG_TOP, BG_BOT, y / (S - 1)))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, S - 1, S - 1], radius=int(S * 0.22), fill=255)
    img.paste(grad, (0, 0), mask)

    c = S / 2.0
    R_ring = S * 0.40          # bearing-ring radius
    R_long = S * 0.365         # cardinal point tip radius
    R_short = R_long * 0.46    # inter-cardinal point tip radius
    R_valley = S * 0.085       # star valley radius (facet width)

    lw = max(1, int(S * 0.012))

    # Bearing ring.
    d.ellipse([c - R_ring, c - R_ring, c + R_ring, c + R_ring],
              outline=RING + (235,), width=lw)

    # Tick marks around the ring: long at the 8 principal bearings, short
    # between them (every 22.5 deg).
    for k in range(16):
        ang = k * 22.5
        ux, uy = _u(ang)
        inner = R_ring - (S * 0.055 if k % 2 == 0 else S * 0.03)
        d.line([(c + ux * inner, c + uy * inner),
                (c + ux * R_ring, c + uy * R_ring)],
               fill=RING + (235,), width=lw)

    # Compass rose: 8 points. Tips every 45 deg (cardinals long, diagonals
    # short); valleys every 45 deg offset by 22.5. Each point is two facets
    # (a lit and a shaded triangle) meeting on its axis for a 3-D look.
    def tip_radius(k):
        return R_long if k % 2 == 0 else R_short

    for k in range(8):
        ang = k * 45.0
        tip = (c + _u(ang)[0] * tip_radius(k), c + _u(ang)[1] * tip_radius(k))
        vprev = (c + _u(ang - 22.5)[0] * R_valley,
                 c + _u(ang - 22.5)[1] * R_valley)
        vnext = (c + _u(ang + 22.5)[0] * R_valley,
                 c + _u(ang + 22.5)[1] * R_valley)
        north = (k == 0)
        light = NORTH_LIGHT if north else STAR_LIGHT
        dark = NORTH_DARK if north else STAR_DARK
        # Lit facet on the left/leading side, shaded on the trailing side.
        d.polygon([(c, c), vprev, tip], fill=light + (255,))
        d.polygon([(c, c), tip, vnext], fill=dark + (255,))

    # Centre hub: gold ring with a deep-navy core.
    rh = S * 0.10
    d.ellipse([c - rh, c - rh, c + rh, c + rh], fill=HUB_GOLD + (255,))
    rc = S * 0.045
    d.ellipse([c - rc, c - rc, c + rc, c + rc], fill=HUB_CORE + (255,))

    return img.resize((size, size), Image.LANCZOS)


def bmp_frame(img):
    """Encode an RGBA image as a 32-bit BGRA ICO/BMP DIB (with empty AND mask)."""
    w, h = img.size
    px = img.load()
    header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = bytearray()
    for y in range(h - 1, -1, -1):          # bottom-up
        for x in range(w):
            r, g, b, a = px[x, y]
            xor += bytes((b, g, r, a))
    and_row = ((w + 31) // 32) * 4           # 1bpp, 32-bit aligned rows
    mask = bytes(and_row * h)
    return header + bytes(xor) + mask


def write_ico(frames, path):
    """Assemble frames into an .ico. <=128px as BMP DIB, 256px as PNG."""
    entries, blobs, offset = [], [], 6 + 16 * len(frames)
    for img in frames:
        size = img.size[0]
        if size >= 256:
            buf = io.BytesIO()
            img.save(buf, format="PNG")
            data = buf.getvalue()
        else:
            data = bmp_frame(img)
        b = 0 if size >= 256 else size       # 0 means 256 in the ICO dir
        entries.append(struct.pack("<BBBBHHII", b, b, 0, 0, 1, 32,
                                   len(data), offset))
        blobs.append(data)
        offset += len(data)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(frames)))
        for e in entries:
            f.write(e)
        for bl in blobs:
            f.write(bl)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    outdir = os.path.join(root, "resources")
    os.makedirs(outdir, exist_ok=True)

    sizes = [16, 24, 32, 48, 64, 128, 256]
    frames = [render(s) for s in sizes]

    master = frames[-1]
    master.save(os.path.join(outdir, "appicon.png"), format="PNG")
    write_ico(frames, os.path.join(outdir, "appicon.ico"))
    print("wrote resources/appicon.png and resources/appicon.ico "
          f"({', '.join(str(s) for s in sizes)})")


if __name__ == "__main__":
    main()
