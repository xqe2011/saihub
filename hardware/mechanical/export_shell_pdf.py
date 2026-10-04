"""Create hardware/docs/shell.pdf from assets made by render_shell_docs.py."""
import argparse
import json
from datetime import date
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.pagesizes import A4, landscape
from reportlab.lib.utils import ImageReader
from reportlab.pdfgen import canvas
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
import reportlab

PAGE_W, PAGE_H = landscape(A4)
INK = colors.HexColor("#24332f")
MUTED = colors.HexColor("#5a6963")
ACCENT = colors.HexColor("#346654")
LINE = colors.HexColor("#c8d1cb")
SOURCE = "https://polymaker.com/wp-content/uploads/lana-downloads/PolyLite-ABS_TDS_EN_V5.5.pdf"

font_dir = Path("/System/Library/Fonts/Supplemental")
regular, bold = font_dir / "Arial.ttf", font_dir / "Arial Bold.ttf"
if not regular.exists() or not bold.exists():
    font_dir = Path(reportlab.__file__).parent / "fonts"
    regular, bold = font_dir / "Vera.ttf", font_dir / "VeraBd.ttf"
pdfmetrics.registerFont(TTFont("DrawingSans", str(regular)))
pdfmetrics.registerFont(TTFont("DrawingBold", str(bold)))


def text(c, x, y, value, size=10, bold=False, color=INK):
    c.setFillColor(color)
    c.setFont("DrawingBold" if bold else "DrawingSans", size)
    c.drawString(x, y, value)


def lines(c, x, y, values, size=10, leading=16, color=MUTED):
    for i, value in enumerate(values):
        text(c, x, y - i * leading, value, size, color=color)


def page(c, number, title, subtitle):
    c.setFillColor(colors.white)
    c.rect(0, 0, PAGE_W, PAGE_H, fill=1, stroke=0)
    text(c, 36, 558, "SAIHub Mini", 20, True)
    text(c, 220, 560, title, 13)
    text(c, 220, 543, subtitle, 8, color=MUTED)
    c.setStrokeColor(LINE)
    c.setLineWidth(0.6)
    c.line(36, 532, PAGE_W - 36, 532)
    c.line(36, 36, PAGE_W - 36, 36)
    text(c, 36, 22, f"MECHANICAL SHELL  |  {date.today()}  |  Dimensions in mm unless noted", 8, color=MUTED)
    text(c, PAGE_W - 75, 22, f"{number} / 3", 8, color=MUTED)


def image_fit(c, path, x, y, width, height):
    image = ImageReader(str(path))
    iw, ih = image.getSize()
    scale = min(width / iw, height / ih)
    w, h = iw * scale, ih * scale
    c.drawImage(image, x + (width - w) / 2, y + (height - h) / 2, w, h, mask="auto")


def arrow(c, x, y, dx, dy):
    c.line(x, y, x + dx * 4 - dy * 1.4, y + dy * 4 + dx * 1.4)
    c.line(x, y, x + dx * 4 + dy * 1.4, y + dy * 4 - dx * 1.4)


def dim_h(c, x1, x2, object_y, dimension_y, label):
    c.setStrokeColor(MUTED)
    c.setLineWidth(0.45)
    for x in (x1, x2):
        c.line(x, object_y, x, dimension_y + (3 if dimension_y > object_y else -3))
    c.line(x1, dimension_y, x2, dimension_y)
    arrow(c, x1, dimension_y, 1, 0)
    arrow(c, x2, dimension_y, -1, 0)
    c.setFillColor(INK)
    c.setFont("DrawingSans", 8)
    c.drawCentredString((x1 + x2) / 2, dimension_y + 4, label)


def dim_v(c, y1, y2, object_x, dimension_x, label):
    c.setStrokeColor(MUTED)
    c.setLineWidth(0.45)
    for y in (y1, y2):
        c.line(object_x, y, dimension_x + (3 if dimension_x > object_x else -3), y)
    c.line(dimension_x, y1, dimension_x, y2)
    arrow(c, dimension_x, y1, 0, 1)
    arrow(c, dimension_x, y2, 0, -1)
    c.saveState()
    c.translate(dimension_x - 4, (y1 + y2) / 2)
    c.rotate(90)
    c.setFillColor(INK)
    c.setFont("DrawingSans", 8)
    c.drawCentredString(0, 0, label)
    c.restoreState()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "docs/shell.pdf")
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    data = json.loads((args.assets / "drawing-data.json").read_text())
    c = canvas.Canvas(str(args.output), pagesize=landscape(A4))
    c.setTitle("SAIHub Mini - shell drawings and ABS mass estimate")
    c.setAuthor("SAIHub")

    page(c, 1, "Shell and assembly", "CAD-derived previews  |  Exploded views, not to scale")
    text(c, 44, 505, "01  WITHOUT PCB", 11, True)
    text(c, 432, 505, "02  WITH PCB", 11, True)
    image_fit(c, args.assets / "without-pcb.png", 44, 135, 355, 345)
    image_fit(c, args.assets / "with-pcb.png", 432, 135, 365, 345)
    c.setStrokeColor(LINE)
    c.line(414, 150, 414, 500)
    overall = data["assembled_size_mm"]
    text(c, 44, 110, f"{overall[0]:g} x {overall[1]:g} x {overall[2]:g} mm overall", 15, True)
    text(c, 44, 89, "9.0 mm body; QR outline and lettering recessed 0.3 mm", 9)
    text(c, 44, 70, "Four labelled top indicators; USB and button access through the side.", 9, color=MUTED)
    lines(c, 432, 112, [
        "Board: 48.0 x 28.0 x 1.6 mm, shown in assembly orientation.",
        "Vertical separation is for illustration; XY alignment is retained.",
        "Available models shown; button, module and LEDs are illustrative.",
        "Shell mass excludes the PCB, components and QR sticker.",
    ], 9, 14)
    c.showPage()

    page(c, 2, "Three-view drawing", "Third-angle arrangement  |  Scale 3:1 at 100% print size  |  Shell only")
    s = 72 / 25.4 * 3
    x, top_y, front_y = 65, 250, 105
    w, d, h = overall[0] * s, overall[1] * s, overall[2] * s
    c.drawImage(str(args.assets / "top.png"), x, top_y, w, d, mask="auto")
    c.drawImage(str(args.assets / "front.png"), x, front_y, w, h, mask="auto")
    c.drawImage(str(args.assets / "right.png"), 540, front_y, d, h, mask="auto")
    text(c, x, 230, "TOP", 9, True)
    text(c, x, 209, "FRONT / CONNECTOR EDGE", 9, True)
    text(c, 540, 209, "RIGHT SIDE", 9, True)
    dim_h(c, x, x + w, top_y + d, 519, "50.4")
    dim_v(c, top_y, top_y + d, x, 43, "30.4")
    dim_h(c, 540, 540 + d, front_y, 77, "30.4")
    dim_v(c, front_y, front_y + h, x + w, 512, f"{overall[2]:g} overall")
    dim_v(c, front_y, front_y + 9 * s, 540 + d, 815, "9.0 body")
    # QR clear opening is dimensioned inside the otherwise blank sticker bed.
    qr_inner = data["qr_clear_area_mm"][0]
    qr_x = x + (data["qr_center_mm"][0] - qr_inner / 2 + 1.2) * s
    qr_y = top_y + (data["qr_center_mm"][1] - qr_inner / 2 + 1.2) * s
    dim_h(c, qr_x, qr_x + qr_inner * s, qr_y, qr_y + 32, f"{qr_inner:.1f} clear")
    dim_v(c, qr_y, qr_y + qr_inner * s, qr_x + qr_inner * s, qr_x + (qr_inner + 1) * s + 11,
          f"{qr_inner:.1f} clear")
    # Port widths correspond to openings, not the connector bodies.
    for lo, hi, label in ((0, 10.4, "10.4"), (11, 16, "5.0"), (16.48, 47.98, "31.5")):
        dim_h(c, x + (lo + 1.2) * s, x + (hi + 1.2) * s, front_y + 3.8 * s, 77, label)
    text(c, 550, 490, "DETAIL DIMENSIONS", 10, True)
    lines(c, 550, 468, [
        f"QR outline outside: {data['qr_frame_outer_mm'][0]:g} x {data['qr_frame_outer_mm'][1]:g}",
        "Sticker bed: 22.0 x 22.0, flat",
        f"Outline width: {data['qr_frame_width_mm']:g}; recess: {data['qr_frame_depth_mm']:g}",
        "Text recess: 0.3 (roof remaining: 0.7)",
        "Nominal wall / floor / roof: 1.0",
        "Outer corner radius: R1.5",
        "Top edge round: R0.3",
        "Nesting overlap: 1.0",
        "Nesting clearance: 0.2 per side",
        "PCB clearance: 0.2 per side",
        "2 side snaps; 0.25 engagement",
        "4 indicator holes: diameter 1.5; pitch 3.2",
    ], 9, 17)
    lines(c, 550, 245, ["Side opening heights:", "USB 4.2  /  button 3.0  /  header 3.9"], 9, 15)
    text(c, 65, 51, "Dimensions govern. Views show external geometry; internal edges are omitted.", 8, color=MUTED)
    c.showPage()

    page(c, 3, "ABS mass and fit notes", "Calculated from the final CAD solids  |  No printer-specific slicing performed")
    total = data["total_abs_mass_g"]
    text(c, 44, 480, f"{total:.2f} g", 42, True, ACCENT)
    text(c, 44, 454, "Nominal finished shell mass", 13)
    text(c, 44, 431, "Base + cover, ABS at 1.04 g/cm3", 10, color=MUTED)
    text(c, 390, 484, "CALCULATION", 10, True)
    lines(c, 390, 462, [
        "Mass = CAD material volume / 1000 x ABS density",
        f"Total material volume: {(data['base']['volume_mm3'] + data['cover']['volume_mm3']) / 1000:.5f} cm3",
        "1 mm skins and walls assumed fully filled by extrusion.",
        "The empty enclosure cavity is already excluded from volume.",
    ], 10, 18)
    c.setFillColor(colors.HexColor("#edf2ef"))
    c.rect(44, 353, 754, 28, fill=1, stroke=0)
    for cx, label in ((56, "PART"), (300, "MATERIAL VOLUME"), (570, "ESTIMATED ABS MASS")):
        text(c, cx, 362, label, 9, True)
    for y, key, name in ((329, "base", "Base"), (297, "cover", "Cover")):
        text(c, 56, y, name, 11)
        text(c, 300, y, f"{data[key]['volume_mm3'] / 1000:.3f} cm3", 11)
        text(c, 570, y, f"{data[key]['abs_mass_g']:.2f} g", 11)
    c.setStrokeColor(LINE)
    c.line(44, 282, 798, 282)
    text(c, 56, 263, "TOTAL", 11, True)
    text(c, 300, 263, f"{(data['base']['volume_mm3'] + data['cover']['volume_mm3']) / 1000:.3f} cm3", 11, True)
    text(c, 570, 263, f"{total:.2f} g", 11, True)
    text(c, 44, 219, "PRINT ESTIMATE", 10, True)
    lines(c, 44, 199, [
        "Excludes supports, brim, purge, PCB, components and sticker.",
        "Actual mass varies with filament density, line width, gaps and flow.",
        "Do not multiply this thin-wall estimate by a general infill percentage.",
        "Use the slicer's filament mass for the final chosen print settings.",
    ], 9, 16)
    text(c, 444, 219, "FIT CHECKS", 10, True)
    lines(c, 444, 199, [
        "Valid connected solids; closed STL meshes.",
        "No overlaps with the board or available component references.",
        "Some component models are missing or illustrative.",
        "Prototype-test side snaps, solder tails and repeated lid removal.",
        "Release: press both side windows, then lift the cover.",
        "Snap arms: 7.5 x 0.6 x 1.2 mm; vertical play: 0.2 mm.",
    ], 9, 16)
    text(c, 44, 103, "Density source: PolyLite ABS Technical Data Sheet, V5.5, 1.04 g/cm3 at 23 C.", 9)
    text(c, 44, 85, "Open manufacturer data sheet", 9, color=ACCENT)
    c.linkURL(SOURCE, (44, 82, 220, 95), relative=0)
    text(c, 44, 62, "Geometry source: hardware/mechanical/enclosure.py and reference/pcb.step. Nominal, not production-toleranced.", 8, color=MUTED)
    c.save()
    print(args.output)


if __name__ == "__main__":
    main()
