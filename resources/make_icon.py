"""Generate descry.ico (and per-size PNGs) from a procedural design.

Design: gradient amber->teal rounded-square backdrop, geometric "D"
in cream with a small relocated dot accent inside the bowl.

Run from repo root:  python resources/make_icon.py
Outputs:             resources/descry.ico  +  resources/icon_<N>.png
"""

from __future__ import annotations

import os
from PIL import Image, ImageDraw

OUT_DIR = os.path.join(os.path.dirname(__file__))

SIZES = [16, 24, 32, 48, 64, 128, 256]

# Render at 4x oversample then downsample for smooth edges. Skip oversampling
# for tiny sizes — we want the geometry to snap to whole pixels.
SUPERSAMPLE = {16: 4, 24: 4, 32: 4, 48: 4, 64: 4, 128: 4, 256: 4}

# Palette
AMBER = (200, 145, 95)         # #C8915F  warm top-left
TEAL = (31, 59, 71)            # #1F3B47  cool bottom-right
CREAM = (244, 236, 224)        # #F4ECE0  letterform
CREAM_GLOW = (255, 248, 232)   # softer halo for the dot at large sizes


def lerp(a: int, b: int, t: float) -> int:
    return int(a + (b - a) * t)


def make_gradient(size: int) -> Image.Image:
    """Diagonal gradient amber (top-left) to teal (bottom-right)."""
    img = Image.new("RGB", (size, size))
    px = img.load()
    for y in range(size):
        for x in range(size):
            t = (x + y) / (2.0 * (size - 1))
            r = lerp(AMBER[0], TEAL[0], t)
            g = lerp(AMBER[1], TEAL[1], t)
            b = lerp(AMBER[2], TEAL[2], t)
            px[x, y] = (r, g, b)
    return img


def rounded_mask(size: int, radius: int) -> Image.Image:
    """Antialiased rounded-square alpha mask."""
    mask = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(mask)
    d.rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=255)
    return mask


def draw_d(img: Image.Image, S: int, draw_dot: bool) -> None:
    """Draw the 'D' letterform centered in the icon, plus optional dot.

    Geometry: stem on the left, bowl as a circle whose center sits ON the
    stem's right edge. The visible right half is the bowl; the (hidden)
    left half tucks behind the stem so they glue flush at top + bottom,
    producing a real D shape rather than a stem-and-detached-circle.
    """
    # Bounding box for the letterform inside the icon.
    pad = int(S * 0.18)
    inner = S - 2 * pad
    stem_w = max(2, int(inner * 0.22))

    # Bowl is a circle whose diameter == letter height. The bowl center
    # sits on the stem's right edge, so the visible right half is a
    # half-disk glued flush to the stem.
    bowl_r = inner // 2
    visible_w = stem_w + bowl_r          # actual rendered letter width
    # Center the letter horizontally inside the bounding box.
    lx = pad + (inner - visible_w) // 2
    ly = pad
    lh = inner

    cx_bowl = lx + stem_w
    cy_bowl = ly + lh // 2

    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)

    # Outer bowl — full circle. We draw onto a temp layer so we can erase
    # the left half cleanly (PIL has no half-disk primitive).
    outer = (
        cx_bowl - bowl_r,
        cy_bowl - bowl_r,
        cx_bowl + bowl_r,
        cy_bowl + bowl_r,
    )
    bowl_layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    bd = ImageDraw.Draw(bowl_layer)
    bd.ellipse(outer, fill=CREAM + (255,))
    # Punch the inner bowl (full ellipse — left half lands in the area
    # we're about to erase, so no harm).
    inner_r = bowl_r - stem_w
    inner_box = (
        cx_bowl - inner_r,
        cy_bowl - inner_r,
        cx_bowl + inner_r,
        cy_bowl + inner_r,
    )
    bd.ellipse(inner_box, fill=(0, 0, 0, 0))
    # Erase the left half so only the right half-disc remains. This is
    # what makes the bowl glue flush to the stem instead of looping
    # around it.
    bd.rectangle(
        (cx_bowl - bowl_r - 1, cy_bowl - bowl_r - 1,
         cx_bowl, cy_bowl + bowl_r + 1),
        fill=(0, 0, 0, 0),
    )

    # Composite the half-bowl onto the letter layer.
    layer = Image.alpha_composite(layer, bowl_layer)
    ld = ImageDraw.Draw(layer)

    # Stem — rectangle from letter's left edge to the bowl edge. Drawn
    # last so it covers the bowl's flat edge cleanly.
    ld.rectangle(
        (lx, ly, cx_bowl, ly + lh),
        fill=CREAM + (255,),
    )

    # Optional dot inside the bowl (relocated i-dot). Skip at small sizes.
    if draw_dot and S >= 32 * 4:
        dot_d = max(2, int(inner * 0.13))
        # Position: upper area of the inner bowl, slightly right of center.
        cx = cx_bowl + int(inner_r * 0.20)
        cy = cy_bowl - int(inner_r * 0.40)
        dot_box = (
            cx - dot_d // 2,
            cy - dot_d // 2,
            cx + dot_d // 2,
            cy + dot_d // 2,
        )
        if S >= 128 * 4:
            halo_d = dot_d * 2
            halo_box = (
                cx - halo_d // 2,
                cy - halo_d // 2,
                cx + halo_d // 2,
                cy + halo_d // 2,
            )
            ld.ellipse(halo_box, fill=CREAM_GLOW + (60,))
        ld.ellipse(dot_box, fill=CREAM + (255,))

    img.alpha_composite(layer)


def render_icon(size: int) -> Image.Image:
    """Render one icon at the requested target size."""
    ss = SUPERSAMPLE.get(size, 4)
    S = size * ss
    radius = int(S * 0.22)

    # Background (gradient + rounded clip)
    bg = make_gradient(S).convert("RGBA")
    mask = rounded_mask(S, radius)
    bg.putalpha(mask)

    # Letterform on top
    draw_dot = size >= 32
    draw_d(bg, S, draw_dot)

    if ss > 1:
        bg = bg.resize((size, size), Image.LANCZOS)
    return bg


def main() -> None:
    pngs = []
    for sz in SIZES:
        img = render_icon(sz)
        out_png = os.path.join(OUT_DIR, f"icon_{sz}.png")
        img.save(out_png, "PNG")
        pngs.append(img)
        print(f"  wrote {out_png}")

    # Multi-size .ico — Pillow takes a base image plus the size list.
    ico_path = os.path.join(OUT_DIR, "descry.ico")
    base = pngs[-1]  # 256x256
    base.save(
        ico_path,
        format="ICO",
        sizes=[(s, s) for s in SIZES],
        append_images=pngs[:-1],
    )
    print(f"  wrote {ico_path}")


if __name__ == "__main__":
    main()
