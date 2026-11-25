"""Convert v113 prototype icons into LVGL RGB565A8 image descriptors."""

from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


ROOT = Path(r"D:\AItools\projects\jt2.0\_work")
SRC = ROOT / "v113_icons"
OUT = ROOT / "esp32p4" / "p4b" / "main" / "APP" / "IMAGE" / "jt_v113_icons.c"
HEADER = ROOT / "esp32p4" / "p4b" / "main" / "APP" / "IMAGE" / "jt_v113_icons.h"
PROTO_ICON_SOURCES = ROOT / "banzi_v113_screens"
GEAR_PIXEL_SOURCE = ROOT / "v113" / "screen1.png"
GEAR_SVG = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <g fill="none" stroke="#3b3d39" stroke-linejoin="round">
    <circle cx="32" cy="32" r="7.5" stroke-width="4"/>
    <path stroke-width="3.6" d="M49.2 35.8a18.2 18.2 0 0 0 0-7.6l4.4-3.4-4.4-7.7-5.2 2.3a18.4 18.4 0 0 0-6.6-3.7L36.8 9.8h-9.6l-1-6.6-5.2 2.3-4.4 7.7 4.4 3.4a18.2 18.2 0 0 0 0 7.6l-4.4 3.4 4.4 7.7 5.2-2.3a18.4 18.4 0 0 0 6.6 3.7l1 6.6h9.6l1-6.6a18.4 18.4 0 0 0 6.6-3.7l5.2 2.3 4.4-7.7-4.4-3.4Z"/>
  </g>
</svg>
"""
GEAR_BOX = (933.9, 14.32, 55.29, 55.29)
GEAR_CANVAS_W = 1024
GEAR_CANVAS_H = 28


def rgba565_le(rgb, alpha):
    r = rgb[:, :, 0].astype(np.uint16) >> 3
    g = rgb[:, :, 1].astype(np.uint16) >> 2
    b = rgb[:, :, 2].astype(np.uint16) >> 3
    value = (r << 11) | (g << 5) | b
    # LV_IMG_CF_RGB565A8 is planar: all RGB565 pixels first, then one
    # alpha byte per pixel. Interleaving RGB565/A8 here renders as noise.
    rgb_le = np.empty((rgb.shape[0], rgb.shape[1], 2), dtype=np.uint8)
    rgb_le[:, :, 0] = value & 0xFF
    rgb_le[:, :, 1] = value >> 8
    alpha_le = alpha[..., None]
    return np.concatenate((rgb_le, alpha_le), axis=2)


def make_c(name, image):
    rgba = np.asarray(image.convert("RGBA"), dtype=np.uint8)
    rgb = rgba[:, :, :3].copy()
    alpha = rgba[:, :, 3].copy()
    data = rgba565_le(rgb, alpha).tobytes()
    lines = [
        f"const uint8_t LV_ATTRIBUTE_LARGE_CONST {name}_map[] = {{\n"
    ]
    for start in range(0, len(data), 32):
        values = ", ".join(f"0x{b:02x}" for b in data[start:start + 32])
        lines.append(f"  {values},\n")
    lines.append("};\n\n")
    lines.append(f"""const lv_img_dsc_t {name} = {{
  .header.cf = LV_IMG_CF_RGB565A8,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = {image.width},
  .header.h = {image.height},
  .data_size = {len(data)},
  .data = {name}_map,
}};

""")
    return "".join(lines)


def make_rgb565_c(name, image):
    """Opaque RGB565 image: no A8 plane, no alpha blending ambiguity."""
    rgb = np.asarray(image.convert("RGB"), dtype=np.uint8)
    r = rgb[:, :, 0].astype(np.uint16) >> 3
    g = rgb[:, :, 1].astype(np.uint16) >> 2
    b = rgb[:, :, 2].astype(np.uint16) >> 3
    value = (r << 11) | (g << 5) | b
    data = np.empty((rgb.shape[0], rgb.shape[1], 2), dtype=np.uint8)
    data[:, :, 0] = value & 0xFF
    data[:, :, 1] = value >> 8
    payload = data.tobytes()
    lines = [f"const uint8_t LV_ATTRIBUTE_LARGE_CONST {name}_map[] = {{\n"]
    for start in range(0, len(payload), 32):
        values = ", ".join(f"0x{b:02x}" for b in payload[start:start + 32])
        lines.append(f"  {values},\n")
    lines.append("};\n\n")
    lines.append(f"""const lv_img_dsc_t {name} = {{
  .header.cf = LV_IMG_CF_TRUE_COLOR,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = {image.width},
  .header.h = {image.height},
  .data_size = {len(payload)},
  .data = {name}_map,
}};

""")
    return "".join(lines)


def make_image_c(name, image):
    rgba = np.asarray(image.convert("RGBA"), dtype=np.uint8)
    rgb = rgba[:, :, :3].copy()
    alpha = rgba[:, :, 3].copy()
    data = rgba565_le(rgb, alpha).tobytes()
    lines = [f"const uint8_t LV_ATTRIBUTE_LARGE_CONST {name}_map[] = {{\n"]
    for start in range(0, len(data), 32):
        values = ", ".join(f"0x{b:02x}" for b in data[start:start + 32])
        lines.append(f"  {values},\n")
    lines.append("};\n\n")
    lines.append(f"""const lv_img_dsc_t {name} = {{
  .header.cf = LV_IMG_CF_RGB565A8,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = {image.width},
  .header.h = {image.height},
  .data_size = {len(data)},
  .data = {name}_map,
}};

""")
    return "".join(lines)


def make_transparent_circle_button(name, draw_glyph, size=(114, 115)):
    scale = 4
    w, h = size[0] * scale, size[1] * scale
    img = Image.new("RGBA", (w, h), (243, 242, 235, 255))
    d = ImageDraw.Draw(img)
    pad = 2 * scale
    d.ellipse((pad, pad, w - 1 - pad, h - 1 - pad), fill=(243, 242, 235, 255))
    draw_glyph(d, w // 2, h // 2, scale)
    img = img.resize(size, Image.Resampling.LANCZOS)
    return make_image_c(name, img)


def draw_back(d, cx, cy, s):
    ink = (17, 18, 17, 255)
    d.line((cx - 25 * s, cy - 2 * s, cx + 8 * s, cy - 2 * s), fill=ink, width=8 * s)
    d.arc((cx - 2 * s, cy - 28 * s, cx + 29 * s, cy + 3 * s),
          start=90, end=270, fill=ink, width=8 * s)
    d.line((cx + 27 * s, cy - 1 * s, cx + 27 * s, cy + 24 * s), fill=ink, width=8 * s)


def draw_confirm(d, cx, cy, s):
    ink = (17, 18, 17, 255)
    d.line((cx - 25 * s, cy + 1 * s, cx - 8 * s, cy + 18 * s), fill=ink, width=8 * s)
    d.line((cx - 8 * s, cy + 18 * s, cx + 27 * s, cy - 20 * s), fill=ink, width=8 * s)


def draw_send(d, cx, cy, s):
    ink = (17, 18, 17, 255)
    d.polygon([(cx - 30 * s, cy + 24 * s), (cx + 34 * s, cy - 4 * s),
               (cx + 2 * s, cy - 30 * s), (cx - 2 * s, cy - 8 * s)], fill=ink)
    d.polygon([(cx - 2 * s, cy - 8 * s), (cx + 34 * s, cy - 4 * s),
               (cx - 4 * s, cy + 4 * s)], fill=ink)
    d.line((cx - 4 * s, cy + 4 * s, cx - 30 * s, cy + 24 * s),
           fill=(243, 242, 235, 255), width=2 * s)


def draw_home(d, cx, cy, s):
    ink = (17, 18, 17, 255)
    d.polygon([(cx - 24 * s, cy - 2 * s), (cx, cy - 25 * s),
               (cx + 24 * s, cy - 2 * s)], fill=ink)
    d.rectangle((cx - 16 * s, cy - 3 * s, cx + 16 * s, cy + 24 * s), fill=ink)
    d.rectangle((cx - 5 * s, cy + 7 * s, cx + 5 * s, cy + 24 * s),
                fill=(243, 242, 235, 255))


def render_gear(size=64):
    """Recover the gear from the actual v113 screenshot pixels.

    The raw HTML SVG renders as a different tooth count once rasterized at
    the board scale; the browser screenshot is the visual source of truth.
    """
    if size and size != 64:
        raise ValueError("gear asset is authored at 64x64")
    if not GEAR_PIXEL_SOURCE.exists():
        raise FileNotFoundError(f"{GEAR_PIXEL_SOURCE} missing")
    shot = Image.open(GEAR_PIXEL_SOURCE).convert("RGB")
    # v113_screen1.png is a 1024x600 browser render. This box is the gear
    # button, measured against the rendered prototype rather than guessed
    # from SVG viewBox math.
    crop = shot.crop((936, 28, 976, 68))
    pixels = np.asarray(crop, dtype=np.float32)
    paper = np.array([233.0, 235.0, 231.0], dtype=np.float32)
    ink = np.array([60.0, 62.0, 58.0], dtype=np.float32)
    distance = np.linalg.norm(pixels - paper, axis=2)
    alpha = np.clip((distance - 4.0) * 3.0, 0, 255).astype(np.uint8)
    alpha_img = Image.fromarray(alpha, "L").resize((64, 64), Image.Resampling.LANCZOS)
    rgba = np.zeros((64, 64, 4), dtype=np.uint8)
    rgba[:, :, :3] = ink.astype(np.uint8)
    rgba[:, :, 3] = np.asarray(alpha_img, dtype=np.uint8)
    return Image.fromarray(rgba, "RGBA")


def render_gear_button(size=64):
    """Opaque v113 gear button bitmap on paper background."""
    if size and size != 64:
        raise ValueError("gear button asset is authored at 64x64")
    if not GEAR_PIXEL_SOURCE.exists():
        raise FileNotFoundError(f"{GEAR_PIXEL_SOURCE} missing")
    shot = Image.open(GEAR_PIXEL_SOURCE).convert("RGB")
    crop = shot.crop((936, 28, 976, 68))
    pixels = np.asarray(crop, dtype=np.float32)
    paper = np.array([233.0, 235.0, 231.0], dtype=np.float32)
    distance = np.linalg.norm(pixels - paper, axis=2)
    alpha = np.clip((distance - 3.0) * 4.0, 0, 255).astype(np.uint8)
    alpha = np.asarray(
        Image.fromarray(alpha, "L").resize((64, 64), Image.Resampling.LANCZOS),
        dtype=np.uint8,
    )
    ink = np.array([60, 62, 58], dtype=np.uint8)
    paper = np.array([233, 235, 231], dtype=np.uint8)
    ratio = (alpha.astype(np.float32) / 255.0)[:, :, None]
    out = (ink[None, None, :] * ratio +
           paper[None, None, :] * (1.0 - ratio)).clip(0, 255).astype(np.uint8)
    return Image.fromarray(out, "RGB")


def main():
    chunks = [
        '#include "lvgl.h"\n\n',
        "/* Generated from v113 prototype SVG buttons. */\n\n",
    ]
    chunks.append(make_image_c("jt_icon_gear", render_gear(64)))
    chunks.append(make_rgb565_c("jt_icon_gear_button", render_gear_button(64)))
    home_source = PROTO_ICON_SOURCES / "jt_v113_4.png"
    back_source = PROTO_ICON_SOURCES / "jt_v113_5.png"
    for name, path, box in (
        ("jt_icon_home_button_idea", home_source, (40, 18, 110, 88)),
        ("jt_icon_home_button", back_source, (42, 20, 106, 84)),
        ("jt_icon_back_button", back_source, (914, 20, 978, 84)),
    ):
        with Image.open(path) as image:
            crop = image.convert("RGB").crop(box).resize((64, 64), Image.Resampling.LANCZOS)
        chunks.append(make_rgb565_c(name, crop))
        crop.save(ROOT / f"{name}_preview.png")
    with Image.open(ROOT / "v113" / "screen2.png") as image:
        shutter = image.convert("RGB").crop((452, 470, 572, 590)).resize(
            (138, 138), Image.Resampling.LANCZOS
        )
    chunks.append(make_rgb565_c("jt_icon_shutter_button", shutter))
    shutter.save(ROOT / "jt_icon_shutter_button_preview.png")
    render_gear_button(64).save(ROOT / "gear_image_button_preview.png")
    render_gear(64).save(ROOT / "gear_generated_preview.png")
    for name in ("back", "confirm"):
        with Image.open(SRC / f"{name}.png") as image:
            chunks.append(make_c(f"jt_icon_{name}", image))
    chunks.append(make_transparent_circle_button("jt_icon_send", draw_send))
    chunks.append(make_transparent_circle_button("jt_icon_home", draw_home))
    OUT.write_text("".join(chunks), encoding="utf-8", newline="\n")
    HEADER.write_text(
        "#ifndef JT_V113_ICONS_H\n"
        "#define JT_V113_ICONS_H\n\n"
        '#include "lvgl.h"\n\n'
        "extern const lv_img_dsc_t jt_icon_gear;\n"
        "extern const lv_img_dsc_t jt_icon_gear_button;\n"
        "extern const lv_img_dsc_t jt_icon_home_button_idea;\n"
        "extern const lv_img_dsc_t jt_icon_home_button;\n"
        "extern const lv_img_dsc_t jt_icon_back_button;\n"
        "extern const lv_img_dsc_t jt_icon_shutter_button;\n"
        "extern const lv_img_dsc_t jt_icon_back;\n"
        "extern const lv_img_dsc_t jt_icon_confirm;\n"
        "extern const lv_img_dsc_t jt_icon_send;\n"
        "extern const lv_img_dsc_t jt_icon_home;\n\n"
        "#endif\n",
        encoding="utf-8",
        newline="\n",
    )
    print(OUT, OUT.stat().st_size)


if __name__ == "__main__":
    main()
