#!/usr/bin/env python3

"""Generate the macKinect app icon source PNG and final .icns bundle."""

from __future__ import annotations

import random
import subprocess
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter


SIZE = 1024
ROOT = Path(__file__).resolve().parent
MASTER_PNG = ROOT / "AppIcon-master.png"
OUTPUT_ICNS = ROOT / "AppIcon.icns"


def lerp(a: int, b: int, t: float) -> int:
    return int(a + (b - a) * t)


def lerp_color(start: tuple[int, int, int], end: tuple[int, int, int], t: float) -> tuple[int, int, int]:
    return tuple(lerp(sa, ea, t) for sa, ea in zip(start, end))


def build_vertical_gradient(size: int, top: tuple[int, int, int], bottom: tuple[int, int, int]) -> Image.Image:
    gradient = Image.new("RGBA", (size, size))
    draw = ImageDraw.Draw(gradient)
    for y in range(size):
        color = lerp_color(top, bottom, y / float(size - 1))
        draw.line((0, y, size, y), fill=(*color, 255))
    return gradient


def blur_ellipse(size: int, bounds: tuple[int, int, int, int], fill: tuple[int, int, int, int], radius: int) -> Image.Image:
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    draw.ellipse(bounds, fill=fill)
    return layer.filter(ImageFilter.GaussianBlur(radius))


def _create_background() -> Image.Image:
    tile_mask = Image.new("L", (SIZE, SIZE), 0)
    tile_draw = ImageDraw.Draw(tile_mask)
    tile_draw.rounded_rectangle((44, 44, SIZE - 44, SIZE - 44), radius=228, fill=255)
    background = build_vertical_gradient(SIZE, (6, 19, 33), (18, 55, 79))
    background.putalpha(tile_mask)
    layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    layer.alpha_composite(background)
    layer.alpha_composite(blur_ellipse(SIZE, (180, 100, 860, 700), (52, 210, 255, 120), 110))
    layer.alpha_composite(blur_ellipse(SIZE, (120, 520, 900, 980), (255, 146, 64, 52), 150))
    layer.alpha_composite(blur_ellipse(SIZE, (90, 860, 934, 1030), (8, 12, 20, 200), 55))
    return layer


def _create_scan_grid() -> Image.Image:
    detail = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(detail)
    # Subtle scanning grid in the lower half keeps the icon tied to depth/scan
    # output without overpowering the Kinect silhouette at small sizes.
    for i in range(8):
        y = 620 + i * 38
        alpha = 26 if i % 2 == 0 else 18
        draw.rounded_rectangle((180, y, 844, y + 3), radius=2, fill=(118, 229, 255, alpha))
    for i in range(9):
        x = 208 + i * 76
        draw.rounded_rectangle((x, 626, x + 2, 916), radius=2, fill=(105, 216, 255, 18))
    return detail


def _create_sensor_shadow() -> Image.Image:
    sensor_shadow = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(sensor_shadow)
    draw.rounded_rectangle((174, 218, 850, 446), radius=110, fill=(0, 0, 0, 190))
    return sensor_shadow.filter(ImageFilter.GaussianBlur(26))


def _create_sensor() -> Image.Image:
    sensor = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(sensor)
    draw.rounded_rectangle((184, 208, 840, 432), radius=108, fill=(17, 26, 36, 255))
    draw.rounded_rectangle((194, 218, 830, 286), radius=72, fill=(38, 56, 76, 190))
    draw.rounded_rectangle((192, 218, 832, 430), radius=108, outline=(128, 227, 255, 150), width=4)
    return sensor.filter(ImageFilter.GaussianBlur(0.4))


def _create_beam() -> Image.Image:
    beam = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(beam)
    draw.polygon(
        [(420, 410), (604, 410), (780, 820), (244, 820)],
        fill=(74, 232, 255, 54),
    )
    draw.polygon(
        [(448, 430), (576, 430), (690, 762), (334, 762)],
        fill=(15, 190, 255, 54),
    )
    for i in range(7):
        t = i / 6.0
        left = 436 - int(t * 148)
        right = 588 + int(t * 148)
        y = 432 + int(t * 320)
        draw.line((left, y, right, y), fill=(154, 239, 255, 85), width=3)
    return beam.filter(ImageFilter.GaussianBlur(6))


def _point_position(col: int, columns: int, span: int, rng: random.Random) -> int:
    if columns == 1:
        x = 512
    else:
        x = int(512 - span + (2 * span) * (col / (columns - 1)))
    return x + rng.randint(-8, 8)


def _create_point_cloud() -> Image.Image:
    rng = random.Random(42)
    points = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(points)
    for row in range(9):
        t = row / 8.0
        y = 470 + int(t * 290)
        span = 70 + int(t * 250)
        columns = 5 + row * 2
        for col in range(columns):  # skylos: ignore[SKY-P403]
            x = _point_position(col, columns, span, rng)
            y_offset = rng.randint(-6, 6)
            radius = 7 if row < 3 else 6
            color = (255, 170, 82, 220) if (row + col) % 3 == 0 else (132, 236, 255, 210)
            draw.ellipse((x - radius, y + y_offset - radius, x + radius, y + y_offset + radius), fill=color)
    return points.filter(ImageFilter.GaussianBlur(0.4))


def _create_lens_glow() -> Image.Image:
    layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    layer.alpha_composite(blur_ellipse(SIZE, (388, 216, 636, 438), (73, 228, 255, 140), 24))
    layer.alpha_composite(blur_ellipse(SIZE, (228, 256, 380, 404), (164, 235, 255, 65), 18))
    layer.alpha_composite(blur_ellipse(SIZE, (646, 256, 798, 404), (164, 235, 255, 65), 18))
    return layer


def _create_lens_layer() -> Image.Image:
    lens_layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    draw = ImageDraw.Draw(lens_layer)
    draw.ellipse((416, 244, 608, 436), fill=(8, 18, 26, 255), outline=(168, 244, 255, 255), width=8)
    draw.ellipse((452, 280, 572, 400), fill=(18, 153, 198, 255), outline=(212, 252, 255, 220), width=5)
    draw.ellipse((486, 314, 538, 366), fill=(242, 253, 255, 255))
    for bounds in ((248, 270, 354, 376), (670, 270, 776, 376)):
        draw.ellipse(bounds, fill=(12, 18, 27, 255), outline=(200, 229, 240, 210), width=5)
        inset = (bounds[0] + 19, bounds[1] + 19, bounds[2] - 19, bounds[3] - 19)
        draw.ellipse(inset, fill=(98, 126, 145, 255))
    draw.ellipse((786, 292, 816, 322), fill=(255, 149, 58, 255))
    draw.rounded_rectangle((350, 232, 674, 252), radius=10, fill=(255, 255, 255, 48))
    return lens_layer


def build_icon() -> Image.Image:
    image = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    image.alpha_composite(_create_background())
    image.alpha_composite(_create_scan_grid())
    image.alpha_composite(_create_sensor_shadow())
    image.alpha_composite(_create_sensor())
    image.alpha_composite(_create_beam())
    image.alpha_composite(_create_point_cloud())
    image.alpha_composite(_create_lens_glow())
    image.alpha_composite(_create_lens_layer())
    return image


def export_iconset(master: Image.Image, destination: Path) -> None:
    sizes = [
        (16, "icon_16x16.png"),
        (32, "icon_16x16@2x.png"),
        (32, "icon_32x32.png"),
        (64, "icon_32x32@2x.png"),
        (128, "icon_128x128.png"),
        (256, "icon_128x128@2x.png"),
        (256, "icon_256x256.png"),
        (512, "icon_256x256@2x.png"),
        (512, "icon_512x512.png"),
        (1024, "icon_512x512@2x.png"),
    ]
    for pixels, name in sizes:
        resized = master.resize((pixels, pixels), Image.Resampling.LANCZOS)
        resized.save(destination / name)


def main() -> None:
    master = build_icon()
    MASTER_PNG.parent.mkdir(parents=True, exist_ok=True)
    master.save(MASTER_PNG)

    with tempfile.TemporaryDirectory(prefix="mackinect-icon-") as temp_dir:
        iconset_dir = Path(temp_dir) / "AppIcon.iconset"
        iconset_dir.mkdir(parents=True, exist_ok=True)
        export_iconset(master, iconset_dir)
        subprocess.run(
            ["/usr/bin/iconutil", "-c", "icns", str(iconset_dir), "-o", str(OUTPUT_ICNS)],
            check=True,
        )

    print(f"wrote {MASTER_PNG}")  # skylos: ignore[SKY-L009]
    print(f"wrote {OUTPUT_ICNS}")  # skylos: ignore[SKY-L009]


if __name__ == "__main__":
    main()
