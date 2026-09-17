#!/usr/bin/env python3
"""Render the exported planar N5 Wang prototype as a dependency-free SVG."""

from html import escape
from pathlib import Path
import sys


def read_vtk(path):
    words = path.read_text().split()
    at = words.index("POINTS")
    count = int(words[at + 1])
    start = at + 3
    values = list(map(float, words[start:start + 3 * count]))
    points = [tuple(values[i:i + 3]) for i in range(0, len(values), 3)]
    cells = []
    for marker in ("CELLS", "POLYGONS"):
        if marker not in words:
            continue
        cursor = words.index(marker)
        cell_count = int(words[cursor + 1])
        cursor += 3
        for _ in range(cell_count):
            size = int(words[cursor])
            cells.append(tuple(map(int, words[cursor + 1:cursor + 1 + size])))
            cursor += size + 1
        break
    return points, cells


def unique_edges(cells):
    result = set()
    for cell in cells:
        for left in range(len(cell)):
            for right in range(left + 1, len(cell)):
                result.add(tuple(sorted((cell[left], cell[right]))))
    return sorted(result)


def projection(point):
    x, y, z = point
    return x - 0.58 * y, -z + 0.24 * y


def transform(points, x, y, width, height, reference=None):
    projected = [projection(point) for point in points]
    frame = [projection(point) for point in (reference or points)]
    low_x = min(point[0] for point in frame)
    high_x = max(point[0] for point in frame)
    low_y = min(point[1] for point in frame)
    high_y = max(point[1] for point in frame)
    scale = min(width / max(high_x - low_x, 1e-12),
                height / max(high_y - low_y, 1e-12))
    offset_x = x + (width - (high_x - low_x) * scale) * 0.5
    offset_y = y + (height - (high_y - low_y) * scale) * 0.5
    return [(offset_x + (px - low_x) * scale,
             offset_y + (py - low_y) * scale) for px, py in projected]


def line_layer(points, cells, color, opacity, width, cut=None):
    if cut is not None:
        cells = [cell for cell in cells
                 if sum(points[index][0] for index in cell) / len(cell) <= cut]
    return points, [(points[left], points[right])
                    for left, right in unique_edges(cells)], color, opacity, width


def main():
    directory = Path(sys.argv[1] if len(sys.argv) > 1
                     else "artifacts/wang-four-hexahedra-prototype")
    output = Path(sys.argv[2] if len(sys.argv) > 2
                  else directory / "prototype-stages.svg")
    hex_points, hexes = read_vtk(directory / "01-four-hexahedra.vtk")
    dc_points, dc_faces = read_vtk(directory / "02-dual-contour-surface.vtk")
    core_points, core_cells = read_vtk(directory / "03-implicit-tetrahedral-core.vtk")
    all_points, transition_cells = read_vtk(directory / "04-wang-transition.vtk")

    # The exporter has converted the project's cube-bit corners to VTK's
    # perimeter order. Draw only VTK_HEXAHEDRON edges, coalescing shared
    # geometric edges so the subdivision reads as one tetrahedral complex.
    vtk_hex_edges = ((0,1),(1,2),(2,3),(3,0),(4,5),(5,6),(6,7),(7,4),
                     (0,4),(1,5),(2,6),(3,7))
    hex_line_keys = set()
    for cell in hexes:
        for a, b in vtk_hex_edges:
            first, second = hex_points[cell[a]], hex_points[cell[b]]
            hex_line_keys.add(tuple(sorted((first, second))))
    hex_lines = sorted(hex_line_keys)
    cut = sorted(point[0] for point in all_points)[len(all_points) // 2]
    panels = [
        ("1  Tetrahedron → four hexahedra", hex_points,
         [(hex_points, hex_lines, "#5946b2", 0.9, 1.25)], []),
        ("2  Closed noisy-sphere DC surface", dc_points, [], dc_faces),
        ("3  Wang transition around implicit core", all_points,
         [line_layer(all_points, transition_cells, "#d66c19", 0.10, 0.35),
          line_layer(core_points, core_cells, "#17689b", 0.24, 0.42)], []),
        ("4  Published surface / transition / core", all_points,
         [line_layer(all_points, transition_cells, "#d66c19", 0.16, 0.38, cut),
          line_layer(core_points, core_cells, "#17689b", 0.30, 0.46, cut)],
         dc_faces),
    ]

    width, height = 1500, 420
    panel_width = width / 4
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#f7f8fa"/>',
        '<text x="750" y="27" text-anchor="middle" font-family="system-ui,sans-serif" '
        'font-size="18" fill="#17202a">Contained noisy-sphere four-hexahedra Wang prototype</text>',
    ]
    for index, (title, bounds, layers, faces) in enumerate(panels):
        left = index * panel_width
        mapped = transform(bounds, left + 18, 58, panel_width - 36, 315)
        if faces:
            source_mapped = transform(dc_points, left + 18, 58,
                                      panel_width - 36, 315, bounds)
            for face in faces:
                polygon = " ".join(f"{source_mapped[v][0]:.2f},{source_mapped[v][1]:.2f}"
                                   for v in face)
                parts.append(f'<polygon points="{polygon}" fill="#26a7b5" '
                             'fill-opacity="0.34" stroke="#126a73" '
                             'stroke-opacity="0.46" stroke-width="0.35"/>')
        for source_points, lines, color, opacity, stroke_width in layers:
            layer_mapped = transform(source_points, left + 18, 58,
                                     panel_width - 36, 315, bounds)
            lookup = {point: layer_mapped[i] for i, point in enumerate(source_points)}
            for first, second in lines:
                a, b = lookup[first], lookup[second]
                parts.append(f'<line x1="{a[0]:.2f}" y1="{a[1]:.2f}" '
                             f'x2="{b[0]:.2f}" y2="{b[1]:.2f}" stroke="{color}" '
                             f'stroke-opacity="{opacity}" stroke-width="{stroke_width}"/>')
        parts.append(f'<text x="{left + panel_width / 2:.1f}" y="402" '
                     'text-anchor="middle" font-family="system-ui,sans-serif" '
                     f'font-size="13" fill="#17202a">{escape(title)}</text>')
        if index:
            parts.append(f'<line x1="{left:.1f}" y1="48" x2="{left:.1f}" y2="405" '
                         'stroke="#ccd1d1" stroke-width="1"/>')
    parts.append('</svg>')
    output.write_text("\n".join(parts) + "\n")


if __name__ == "__main__":
    main()
