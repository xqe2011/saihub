"""Parametric SAIHub-Mini enclosure. All dimensions are millimetres.

Run: python enclosure.py [--output PATH] [--preview]
Coordinate system: label face up, connector edge at -Y, USB at left.
PCB coordinates map to (48 - x, y, PCB_Z); see README for fit assumptions.
"""

import argparse
from collections import Counter
import json
from pathlib import Path
import struct

from build123d import Align, Box, Color, Compound, Cylinder, FontStyle, Plane, Polygon, Pos, RectangleRounded, Rot, Text, export_step, export_stl, extrude, fillet, import_step

BOARD_W, BOARD_D, BOARD_T = 48.0, 28.0, 1.6
WALL = 1.0
RIM = 1.0
FIT = 0.2  # Per-side sliding clearance, not a guaranteed friction fit.
BOARD_GAP = 0.2
FLOOR = 1.0
PCB_Z = 2.8  # 1.8 mm above the floor, clearing the available connector-tail model.
SEAM_Z = 7.5
LAP = 1.0
ROOF_Z = 8.0  # Clears the available USB body model by 0.295 mm under a solid 1 mm roof.
TEXT_DEPTH = 0.3  # Recessed lettering leaves 0.7 mm of the 1 mm roof.
QR_INNER = 22.0
QR_FRAME_WIDTH = 0.18  # Matches the ~0.177 mm stem of the 1.8 mm regular lettering.
QR_OUTER = QR_INNER + 2 * QR_FRAME_WIDTH
QR_CENTER = (35.5, 16.8)
CENTER = (BOARD_W / 2, BOARD_D / 2)
SOCKET = (BOARD_W + 2 * BOARD_GAP, BOARD_D + 2 * BOARD_GAP)
OUTER = tuple(d + 2 * WALL for d in SOCKET)
LIP_OUTER = tuple(d - 2 * FIT for d in SOCKET)
LIP_INNER = tuple(d - 2 * RIM for d in LIP_OUTER)
CORNER_RADIUS = 1.5  # Larger inner radii would clip the board's square corners.
QR_FRAME_DEPTH = TEXT_DEPTH  # Same engraving depth as the lettering.
# One aligned bank, read from the same component placement used by the PCB.
INDICATOR_LABELS = {'D4':'3V3 OUT', 'D5':'5V OUT', 'D6':'5V IN', 'D7':'STATUS'}

def indicators():
    design = json.loads((Path(__file__).parents[1]/'agent/design.json').read_text())
    parts = {a['ref']:a for a in design['parts']}
    return [(ref, label, BOARD_W-parts[ref]['pos'][0], parts[ref]['pos'][1])
            for ref, label in INDICATOR_LABELS.items()]



def block(w, d, h, x=24.0, y=14.0, z=0.0):
    return Pos(x, y, z) * Box(w, d, h, align=(Align.CENTER, Align.CENTER, Align.MIN))


def rounded_block(size, height, z=0.0):
    inset = (OUTER[0] - size[0]) / 2
    radius = max(0.15, CORNER_RADIUS - inset)
    return Pos(*CENTER, z) * extrude(RectangleRounded(*size, radius), amount=height)


def ring(outer, inner, z, height):
    return rounded_block(outer, height, z) - rounded_block(inner, height + 0.02, z - 0.01)


def text_cut(value, x, y, size=2.5, rotation=0, anchor="center", bold=False):
    sketch = Text(value, font_size=size, font="Helvetica", font_style=FontStyle.BOLD if bold else FontStyle.REGULAR)
    lettering = Rot(0, 0, rotation) * extrude(sketch, amount=TEXT_DEPTH + 0.01)
    bounds = lettering.bounding_box()
    dx = -bounds.min.X if anchor == "left" else -(bounds.min.X + bounds.max.X) / 2
    dy = -bounds.min.Y if anchor == "bottom" else -(bounds.min.Y + bounds.max.Y) / 2
    return Pos(x + dx, y + dy, ROOF_Z + WALL - TEXT_DEPTH) * lettering


def connector_clearances():
    # Centres derive from the committed board placement, rotated to match the sketch.
    # Openings extend through both the large socket and the smaller cover lip.
    return [
        # Side access only: stop every cut at or below the roof underside.
        block(10.4, 10.2, ROOF_Z - (PCB_Z + 1.0),
              x=48 - 42.8, y=3.0, z=PCB_Z + 1.0),
        block(5.0, 5.4, 3.0, x=48 - 34.5, y=0.5, z=PCB_Z + 1.0),
        # Open to the base seam: remove the fragile 0.3 mm bar above the header.
        block(31.5, 12, SEAM_Z - (PCB_Z + 0.8),
              x=48 - (1.8 + 11 * 2.54 / 2), y=-2, z=PCB_Z + 0.8),
    ]


def make_enclosure():
    base = rounded_block(OUTER, FLOOR)
    base += ring(OUTER, tuple(d - 2 * WALL for d in OUTER), FLOOR, SEAM_Z - FLOOR)

    # Four short ledges support the PCB edges, away from the open board notch.
    # The nearby side walls locate the PCB with 0.2 mm clearance.
    for x in (-0.4, 48.4):
        for y in (10.0, 17.0):
            base += block(1.6, 2.0, PCB_Z - FLOOR, x=x, y=y, z=FLOOR)

    cover = rounded_block(OUTER, WALL, ROOF_Z)
    # A small rolled top edge catches light without consuming the 1 mm roof.
    top_edges = [edge for edge in cover.edges() if abs(edge.center().Z - (ROOF_Z + WALL)) < 1e-6]
    cover = fillet(top_edges, radius=0.3)
    cover += ring(OUTER, tuple(d - 2 * WALL for d in OUTER), SEAM_Z, ROOF_Z - SEAM_Z)
    # The inward lip starts above the PCB; verify local edge-component clearance.
    cover += ring(OUTER, LIP_INNER, SEAM_Z, ROOF_Z - SEAM_Z)
    cover += ring(LIP_OUTER, LIP_INNER, SEAM_Z - LAP, LAP)

    # Small ceiling fingers limit board lift to 0.3 mm without touching the notch.
    for x in (0.2, 47.8):
        for y in (8.0, 19.0):
            cover += block(0.6, 2.0, ROOF_Z - (PCB_Z + BOARD_T + 0.3),
                           x=x, y=y, z=PCB_Z + BOARD_T + 0.3)

    for opening in connector_clearances():
        base -= opening
        cover -= opening

    # Opposed side cantilevers retain the lid vertically. Each hook cams inward
    # 0.25 mm during insertion and catches a side window; press both to release.
    for right in (False, True):
        x = 47.7 if right else 0.3
        # Free the arm from the nesting lip, retaining a 0.4 mm gap above it.
        cover -= block(0.8, 7.8, 0.5, x=x, y=14.1, z=6.4)
        cover += block(0.6, 7.5, 1.2, x=x, y=14.25, z=5.3)
        cover += block(0.6, 1.5, 2.3, x=x, y=18.75, z=5.3)
        # Free end hook: flat upper catch and sloping lower insertion face.
        outline = [(0, 5.3), (-0.45, 5.9), (-0.45, 6.5), (0.6, 6.5), (0.6, 5.3)]
        if right:
            outline = [(48 - u, v) for u, v in outline]
        profile = Plane.XZ * Polygon(*outline, align=None)
        cover += Pos(0, 10.5, 0) * extrude(profile, amount=1.5, dir=(0, 1, 0))
        base -= block(2.0, 2.1, 1.5, x=48.7 if right else -0.7, y=11.25, z=5.2)

    # Intaglio border with a square, full-size sticker bed flush with the cover.
    qr_z = ROOF_Z + WALL - QR_FRAME_DEPTH
    qr = Pos(*QR_CENTER, qr_z) * extrude(
        RectangleRounded(QR_OUTER, QR_OUTER, QR_FRAME_WIDTH), amount=QR_FRAME_DEPTH + 0.01)
    qr -= block(QR_INNER, QR_INNER, QR_FRAME_DEPTH + 0.03, x=QR_CENTER[0], y=QR_CENTER[1], z=qr_z - 0.01)
    cover -= qr
    # A quiet left-aligned wordmark and model line balance the larger QR area.
    cover -= text_cut("SAIHub", 1.0, 26.0, 3.5, anchor="left", bold=True)
    cover -= text_cut("MINI", 1.0, 21.7, 1.8, anchor="left")
    # Physical pin order, reversed in this face view. User-facing channels 1–8
    # correspond to the board's IO0–IO7. Every label gets one 2.54 mm pitch column.
    pin_labels = ("3.3V", "GND", "5V", "GND", "1", "2", "3", "4", "5", "6", "7", "8")
    for i, label in enumerate(pin_labels):
        cover -= text_cut(label, 48 - (1.8 + 2.54 * i), 0.2, 1.8, rotation=90, anchor="bottom")

    # Individual wells prevent direct lateral light paths between adjacent lamps.
    # Well bottoms clear the 0.55 mm LEDs by 0.4 mm; prototype visibility still needs checking.
    well_bottom = PCB_Z + BOARD_T + 0.95
    for ref, label, x, y in indicators():
        cover += Pos(x, y, well_bottom) * Cylinder(1.1, ROOF_Z-well_bottom+0.02, align=(Align.CENTER, Align.CENTER, Align.MIN))
        cover -= Pos(x, y, well_bottom-0.02) * Cylinder(0.75, ROOF_Z+WALL-well_bottom+0.04, align=(Align.CENTER, Align.CENTER, Align.MIN))
        cover -= text_cut(label, x-10.0, y, 1.6, anchor="left")

    base.label, cover.label = "Base", "Cover"
    base.color, cover.color = Color(0.70, 0.71, 0.70), Color(0.82, 0.83, 0.82)
    return base, cover


def board_reference():
    outline = Polygon((48, 0), (0, 0), (0, 28), (26.15, 28), (26.15, 20.5), (48, 20.5), align=None)
    board = Pos(0, 0, PCB_Z) * extrude(outline, amount=BOARD_T, dir=(0, 0, 1))
    board.label, board.color = "PCB envelope (reference only)", Color(0.05, 0.45, 0.24)
    return board


def populated_board():
    """Board export in assembly coordinates; absent switch/module models use envelopes."""
    source = import_step(Path(__file__).parent / "reference" / "pcb.step")
    parts = []
    for child in source.children:
        part = Pos(48, 0, PCB_Z) * Rot(0, 0, 180) * child
        if child.label.endswith("_PCB"):
            rgb = (0.12, 0.40, 0.27)
        elif child.label.endswith("_pad"):
            rgb = (0.75, 0.62, 0.32)
        elif "USB" in child.label:
            rgb = (0.72, 0.74, 0.76)
        else:
            rgb = (0.24, 0.26, 0.27)
        part.color = Color(*rgb)
        parts.append(part)
    # These two custom footprints have no supplied component models. Their XY
    # dimensions/positions follow the board; the simplified heights are illustrative.
    for name, shape, rgb in [
        ("Button body envelope", block(4.6, 2.2, 1.8, x=13.5, y=1.55, z=PCB_Z + BOARD_T), (0.65, 0.66, 0.67)),
        ("Button actuator envelope", block(2, 0.9, 1.0, x=13.5, y=0, z=PCB_Z + BOARD_T + 0.4), (0.20, 0.21, 0.22)),
        ("Module substrate envelope", block(16, 24, 0.8, x=39.15, y=16, z=PCB_Z + BOARD_T), (0.12, 0.29, 0.22)),
        ("Module shield envelope", block(15, 15, 1.6, x=39.15, y=12, z=PCB_Z + BOARD_T + 0.8), (0.65, 0.67, 0.68)),
    ]:
        shape.label, shape.color = name, Color(*rgb)
        parts.append(shape)
    colors = {'D4':(0.1,0.8,0.2), 'D5':(0.1,0.8,0.2), 'D6':(0.9,0.1,0.1), 'D7':(0.1,0.3,1.0)}
    for ref, label, x, y in indicators():
        led = block(.8, 1.6, .55, x=x, y=y, z=PCB_Z+BOARD_T)
        led.label, led.color = ref+' '+label+' LED envelope', Color(*colors[ref])
        parts.append(led)
    return Compound(label="PCB assembly reference", children=parts)


def intersection_volume(first, second):
    overlap = first.intersect(second)
    if overlap is None:
        return 0.0
    if isinstance(overlap, list):
        return sum(part.volume for part in overlap)
    return overlap.volume


def validate(base, cover, board):
    report = {}
    for name, part in (("base", base), ("cover", cover)):
        assert part.is_valid, f"Invalid {name}"
        assert len(part.solids()) == 1, f"{name} must be one connected solid"
        report[name] = {"volume_mm3": round(part.volume, 3), "solid_count": len(part.solids())}
    for name, first, second in (("base_cover", base, cover), ("base_board", base, board),
                                ("cover_board", cover, board)):
        volume = intersection_volume(first, second)
        assert volume < 1e-6, f"Interference: {name}: {volume} mm³"
        report[name + "_interference_mm3"] = round(volume, 6)
    # Check the engraving floor and the flush sticker bed in the finished solid.
    top_z = ROOF_Z + WALL
    assert abs(cover.bounding_box().max.Z - top_z) < 1e-6
    assert 0 < QR_FRAME_DEPTH < WALL
    for axis in (0, 1):
        for side in (-1, 1):
            point = list(QR_CENTER)
            point[axis] += side * (QR_INNER + QR_OUTER) / 4
            groove = block(0.1, 0.1, QR_FRAME_DEPTH - 0.02, x=point[0], y=point[1],
                           z=top_z - QR_FRAME_DEPTH + 0.01)
            assert intersection_volume(cover, groove) < 1e-6, "QR outline must be recessed"
            floor = block(0.1, 0.1, 0.1, x=point[0], y=point[1], z=top_z - QR_FRAME_DEPTH - 0.1)
            assert abs(intersection_volume(cover, floor) - floor.volume) < 1e-6
    bed = block(QR_INNER, QR_INNER, 0.1, x=QR_CENTER[0], y=QR_CENTER[1], z=top_z - 0.1)
    assert abs(intersection_volume(cover, bed) - bed.volume) < 1e-6, "QR sticker bed must stay flat"
    report.update({"assembled_size_mm": [*[round(d, 3) for d in OUTER], ROOF_Z + WALL],
                   "wall_mm": WALL, "each_mating_rim_mm": RIM, "nesting_clearance_per_side_mm": FIT,
                   "qr_clear_area_mm": [QR_INNER, QR_INNER], "qr_frame_outer_mm": [QR_OUTER, QR_OUTER],
                   "qr_frame_width_mm": QR_FRAME_WIDTH, "qr_frame_depth_mm": QR_FRAME_DEPTH, "qr_center_mm": list(QR_CENTER), "outer_corner_radius_mm": CORNER_RADIUS,
                   "text_engraving_depth_mm": TEXT_DEPTH, "snap_count": 2,
                   "snap_engagement_mm": 0.25, "snap_beam_mm": [7.5, 0.6, 1.2],
                   "component_fit": "Prototype verification required"})
    return report


def validate_components(base, cover, board):
    """Check available geometry, plus conservative fully displaced snap-arm envelopes."""
    result = {}
    for name, shape in [("base", base), ("cover", cover),
                        ("left_snap_insertion", block(0.85, 7.5, 1.2, x=0.425, y=14.25, z=5.3)),
                        ("right_snap_insertion", block(0.85, 7.5, 1.2, x=47.575, y=14.25, z=5.3))]:
        collisions = []
        for child in board.children:
            volume = intersection_volume(shape, child)
            if volume > 1e-6:
                collisions.append({"reference": child.label, "volume_mm3": round(volume, 6)})
        result[name] = collisions
    assert not any(result.values()), f"Component or snap insertion interference: {result}"
    return {"collisions": result,
            "scope": "Available board models and illustrative envelopes; missing models, solder and snap forces unverified."}


def validate_stl(path):
    data = path.read_bytes()
    triangles = struct.unpack_from("<I", data, 80)[0]
    edges = Counter()
    for i in range(triangles):
        values = struct.unpack_from("<12fH", data, 84 + 50 * i)
        vertices = [tuple(round(v, 6) for v in values[j:j + 3]) for j in (3, 6, 9)]
        assert len(set(vertices)) == 3, f"Degenerate mesh triangle in {path.name}"
        for a, b in ((0, 1), (1, 2), (2, 0)):
            edges[tuple(sorted((vertices[a], vertices[b])))] += 1
    assert all(count == 2 for count in edges.values()), f"Non-manifold mesh in {path.name}"
    return triangles


def preview(base, cover, board, output):
    import numpy as np

    from PIL import Image, ImageDraw, ImageFont

    canvas = Image.new("RGB", (2200, 1050), "#f5f5f3")
    draw = ImageDraw.Draw(canvas)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 23)
    except OSError:
        font = ImageFont.load_default(size=23)
    shell_rgb = (0.83, 0.84, 0.83)
    board_lift, cover_lift = 14, 34
    exploded = [(base, shell_rgb, None)]
    exploded += [(Pos(0, 0, board_lift) * child, tuple(child.color)[:3], None) for child in board.children]
    exploded += [(Pos(0, 0, cover_lift) * cover, shell_rgb, ROOF_Z + WALL + cover_lift)]
    for index, (items, title, elevation, azimuth) in enumerate([
        ([(cover, shell_rgb, ROOF_Z + WALL)], "SAIHub Mini · cover", 90, -90),
        (exploded, "COVER / PCB / BASE · exploded assembly", 32, -65),
    ]):
        point_groups, triangle_groups, color_groups = [], [], []
        vertex_offset = 0
        for part, rgb, relief_z in items:
            vertices, mesh_triangles = part.tessellate(0.05)
            mesh_points = np.array([tuple(v) for v in vertices])
            mesh_triangles = np.array(mesh_triangles)
            mesh_colors = np.tile(rgb, (len(mesh_triangles), 1))
            if relief_z is not None:
                height = mesh_points[mesh_triangles].mean(axis=1)[:, 2]
                detail = (height > relief_z + 0.001) | ((height < relief_z - 0.01) & (height > relief_z - max(TEXT_DEPTH, QR_FRAME_DEPTH) - 0.01))
                mesh_colors[detail] = [0.46, 0.48, 0.47]
            point_groups.append(mesh_points)
            triangle_groups.append(mesh_triangles + vertex_offset)
            color_groups.append(mesh_colors)
            vertex_offset += len(mesh_points)
        points = np.concatenate(point_groups)
        triangles = np.concatenate(triangle_groups)
        faces = points[np.array(triangles)]
        normals = np.cross(faces[:, 1] - faces[:, 0], faces[:, 2] - faces[:, 0])
        normals /= np.maximum(np.linalg.norm(normals, axis=1)[:, None], 1e-12)
        shade = 0.55 + 0.4 * np.abs(normals @ np.array([-0.3, -0.4, 0.85]))
        colors = shade[:, None] * np.concatenate(color_groups)
        az, el = np.radians([azimuth, elevation])
        toward = np.array([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)])
        right = np.array([-np.sin(az), np.cos(az), 0])
        up = np.cross(toward, right)
        projected = points @ np.array([right, -up, toward]).T
        lo, hi = projected[:, :2].min(axis=0), projected[:, :2].max(axis=0)
        scale = min(980 / (hi[0] - lo[0]), 840 / (hi[1] - lo[1]))
        projected[:, :2] = (projected[:, :2] - (hi + lo) / 2) * scale + [550, 520]
        pixels = np.full((1000, 1100, 3), 245, dtype=np.uint8)
        depth = np.full((1000, 1100), -np.inf)
        for ids, color in zip(triangles, colors):
            a, b, c = projected[list(ids)]
            xmin, ymin = np.maximum(np.floor(np.minimum(np.minimum(a[:2], b[:2]), c[:2])), 0).astype(int)
            xmax, ymax = np.minimum(np.ceil(np.maximum(np.maximum(a[:2], b[:2]), c[:2])), [1099, 999]).astype(int)
            determinant = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
            if abs(determinant) < 1e-9 or xmax < xmin or ymax < ymin:
                continue
            yy, xx = np.mgrid[ymin:ymax + 1, xmin:xmax + 1] + 0.5
            wa = ((b[1] - c[1]) * (xx - c[0]) + (c[0] - b[0]) * (yy - c[1])) / determinant
            wb = ((c[1] - a[1]) * (xx - c[0]) + (a[0] - c[0]) * (yy - c[1])) / determinant
            wc = 1 - wa - wb
            z = wa * a[2] + wb * b[2] + wc * c[2]
            region = depth[ymin:ymax + 1, xmin:xmax + 1]
            mask = (wa >= -1e-8) & (wb >= -1e-8) & (wc >= -1e-8) & (z > region)
            region[mask] = z[mask]
            pixels[ymin:ymax + 1, xmin:xmax + 1][mask] = np.clip(color * 255, 0, 255).astype(np.uint8)
        canvas.paste(Image.fromarray(pixels), (index * 1100, 50))
        draw.text((index * 1100 + 550, 30), title, font=font, fill="#28333e", anchor="mm")
    canvas.save(output / "preview.png")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(__file__).parent / "output")
    parser.add_argument("--preview", action="store_true")
    parser.add_argument("--check-components", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    base, cover = make_enclosure()
    board = board_reference()
    populated = populated_board()
    report = validate(base, cover, board)
    lamps = indicators()
    assert len(lamps) == 4 and len({round(x, 4) for _, _, x, _ in lamps}) == 1
    assert all(abs(lamps[i+1][3]-lamps[i][3]-3.2)<1e-6 for i in range(3))
    report['indicator_bank'] = {'labels':[label for _,label,_,_ in lamps],
                                'opening_diameter_mm':1.5, 'well_outer_diameter_mm':2.2,
                                'led_top_clearance_mm':0.4, 'pitch_mm':3.2}
    if args.check_components:
        checks = validate_components(base, cover, populated)
        (args.output / "component-clearance.json").write_text(json.dumps(checks, indent=2) + "\n")
    for name, part in (("base", base), ("cover", cover)):
        assert export_step(part, args.output / f"{name}.step")
        # Both exports retain assembly coordinates; slicers can place each on the bed.
        assert export_stl(part, args.output / f"{name}.stl", tolerance=0.02, angular_tolerance=0.15)
        report[name]["stl_triangles"] = validate_stl(args.output / f"{name}.stl")
        report[name]["stl_closed"] = True
    export_step(Compound(label="SAIHub-Mini shell", children=[base, cover]), args.output / "assembly.step")
    export_step(Compound(label="Shell with PCB reference", children=[base, cover, populated]),
                args.output / "fit-reference.step")
    (args.output / "validation.json").write_text(json.dumps(report, indent=2) + "\n")
    if args.preview:
        preview(base, cover, populated, args.output)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
