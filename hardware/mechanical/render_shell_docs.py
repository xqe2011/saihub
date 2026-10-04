"""Render CAD-derived shell drawing views. Run before export_shell_pdf.py."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image
from build123d import Pos
import enclosure as model


def render(items, elevation, azimuth, path):
    point_groups, triangle_groups, color_groups = [], [], []
    vertex_offset = 0
    for part, rgb, relief_z in items:
        vertices, mesh_triangles = part.tessellate(0.05)
        mesh_points = np.array([tuple(v) for v in vertices])
        mesh_triangles = np.array(mesh_triangles)
        mesh_colors = np.tile(rgb, (len(mesh_triangles), 1))
        if relief_z is not None:
            height = mesh_points[mesh_triangles].mean(axis=1)[:, 2]
            detail = (height > relief_z + 0.001) | ((height < relief_z - 0.01) & (height > relief_z - max(model.TEXT_DEPTH, model.QR_FRAME_DEPTH) - 0.01))
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
    span = hi - lo
    scale = 1600 / max(span)
    width, height = np.ceil(span * scale).astype(int)
    projected[:, :2] = (projected[:, :2] - lo) * scale
    pixels = np.full((height, width, 3), 255, dtype=np.uint8)
    depth = np.full((height, width), -np.inf)
    for ids, color in zip(triangles, colors):
        a, b, c = projected[list(ids)]
        xmin, ymin = np.maximum(np.floor(np.minimum(np.minimum(a[:2], b[:2]), c[:2])), 0).astype(int)
        xmax, ymax = np.minimum(np.ceil(np.maximum(np.maximum(a[:2], b[:2]), c[:2])), [width - 1, height - 1]).astype(int)
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
    if elevation in (0, 90):
        # Depth shading distinguishes walls/floor seen through side and indicator openings.
        finite = np.isfinite(depth)
        factor = np.ones_like(depth)
        factor[finite] = np.clip(1 - (depth[finite].max() - depth[finite]) * 0.03, 0.35, 1)
        pixels = (pixels * factor[:, :, None]).astype(np.uint8)
    Image.fromarray(pixels).save(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    base, cover = model.make_enclosure()
    board = model.populated_board()
    report = model.validate(base, cover, model.board_reference())
    shell = (0.83, 0.84, 0.83)
    assembled = [(base, shell, None), (cover, shell, model.ROOF_Z + model.WALL)]
    without = [(base, shell, None), (Pos(0, 0, 22) * cover, shell, model.ROOF_Z + model.WALL + 22)]
    with_board = [(base, shell, None)]
    with_board += [(Pos(0, 0, 14) * p, tuple(p.color)[:3], None) for p in board.children]
    with_board += [(Pos(0, 0, 34) * cover, shell, model.ROOF_Z + model.WALL + 34)]
    for name, parts, el, az in [
        ("without-pcb", without, 32, -65), ("with-pcb", with_board, 32, -65),
        ("top", assembled, 90, -90), ("front", assembled, 0, -90), ("right", assembled, 0, 0),
    ]:
        render(parts, el, az, args.output / f"{name}.png")
        print("Rendered", name, flush=True)
    report["abs_density_g_cm3"] = 1.04
    for name, part in (("base", base), ("cover", cover)):
        report[name]["volume_mm3"] = part.volume
        report[name]["abs_mass_g"] = part.volume / 1000 * report["abs_density_g_cm3"]
    report["total_abs_mass_g"] = report["base"]["abs_mass_g"] + report["cover"]["abs_mass_g"]
    (args.output / "drawing-data.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
