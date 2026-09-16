#!/usr/bin/env python3
"""Adapt the finite noisy DC/core PLC to Diazzi et al.'s OFF-only CDT CLI.

This is an offline research harness.  It does not make CDT a project
dependency and deliberately leaves geometry/quality acceptance to the existing
complete-volume verifier after conversion to TetGen's simple node/element
format.  The input is the `.poly` emitted by `export_complete_volume.cpp`.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import random
import shutil
import subprocess
import sys
from collections import Counter


def read_poly(path: pathlib.Path) -> tuple[list[tuple[float, float, float]], list[tuple[tuple[int, int, int], int]]]:
    lines = iter(path.read_text(encoding="utf-8").splitlines())
    header = next(lines).split()
    if len(header) < 4 or header[1:4] != ["3", "0", "0"]:
        raise ValueError("expected a 3-D, attribute-free .poly point header")
    count = int(header[0])
    vertices: list[tuple[float, float, float]] = []
    for expected_id in range(count):
        fields = next(lines).split()
        if len(fields) != 4 or int(fields[0]) != expected_id:
            raise ValueError("expected sequential point IDs")
        vertices.append((float(fields[1]), float(fields[2]), float(fields[3])))
    facet_header = next(lines).split()
    if len(facet_header) != 2 or facet_header[1] != "1":
        raise ValueError("expected one facet marker")
    facets: list[tuple[tuple[int, int, int], int]] = []
    for _ in range(int(facet_header[0])):
        polygon_header = next(lines).split()
        triangle = next(lines).split()
        if len(polygon_header) != 3 or polygon_header[0:2] != ["1", "0"]:
            raise ValueError("expected one triangular polygon and no facet holes")
        if len(triangle) != 4 or triangle[0] != "3":
            raise ValueError("expected triangular facet")
        face = tuple(int(value) for value in triangle[1:4])
        if any(index < 0 or index >= count for index in face):
            raise ValueError("facet index outside point range")
        facets.append((face, int(polygon_header[2])))
    return vertices, facets


def write_off(path: pathlib.Path, vertices: list[tuple[float, float, float]], facets: list[tuple[tuple[int, int, int], int]], vertex_order: list[int] | None = None, facet_order: list[int] | None = None) -> None:
    vertex_order = list(range(len(vertices))) if vertex_order is None else vertex_order
    facet_order = list(range(len(facets))) if facet_order is None else facet_order
    inverse = {old: new for new, old in enumerate(vertex_order)}
    with path.open("w", encoding="utf-8") as output:
        output.write(f"OFF\n{len(vertices)} {len(facets)} 0\n")
        for index in vertex_order:
            x, y, z = vertices[index]
            output.write(f"{x:.17g} {y:.17g} {z:.17g}\n")
        for index in facet_order:
            (a, b, c), _ = facets[index]
            output.write(f"3 {inverse[a]} {inverse[b]} {inverse[c]}\n")


def orient_closed_components(
    vertices: list[tuple[float, float, float]], facets: list[tuple[tuple[int, int, int], int]]
) -> list[tuple[tuple[int, int, int], int]]:
    """Give the outer component outward and every enclosed component inward winding."""
    edge_uses: dict[tuple[int, int], list[tuple[int, tuple[int, int]]]] = {}
    for face_index, (face, _) in enumerate(facets):
        for index in range(3):
            edge = (face[index], face[(index + 1) % 3])
            edge_uses.setdefault(tuple(sorted(edge)), []).append((face_index, edge))
    if any(len(uses) != 2 for uses in edge_uses.values()):
        raise ValueError("OFF adapter requires closed two-manifold facet components")
    flipped: list[bool | None] = [None] * len(facets)
    components: list[list[int]] = []
    for seed in range(len(facets)):
        if flipped[seed] is not None:
            continue
        flipped[seed] = False
        component: list[int] = []
        pending = [seed]
        while pending:
            current = pending.pop()
            component.append(current)
            face = facets[current][0]
            for edge_index in range(3):
                original_edge = (face[edge_index], face[(edge_index + 1) % 3])
                directed_edge = original_edge[::-1] if flipped[current] else original_edge
                uses = edge_uses[tuple(sorted(original_edge))]
                neighbor, neighbor_edge = uses[1] if uses[0][0] == current else uses[0]
                expected_flip = neighbor_edge == directed_edge
                if flipped[neighbor] is None:
                    flipped[neighbor] = expected_flip
                    pending.append(neighbor)
                elif flipped[neighbor] != expected_flip:
                    raise ValueError("inconsistent facet orientation cycle")
        components.append(component)
    oriented = list(facets)
    component_volumes: list[float] = []
    for component in components:
        volume = 0.0
        for face_index in component:
            a, b, c = oriented[face_index][0]
            if flipped[face_index]:
                b, c = c, b
            oriented[face_index] = ((a, b, c), oriented[face_index][1])
            volume += dot(vertices[a], cross(vertices[b], vertices[c])) / 6.0
        component_volumes.append(volume)
    outer = max(range(len(components)), key=lambda index: abs(component_volumes[index]))
    for component_index, component in enumerate(components):
        should_be_positive = component_index == outer
        if (component_volumes[component_index] > 0.0) != should_be_positive:
            for face_index in component:
                a, b, c = oriented[face_index][0]
                oriented[face_index] = ((a, c, b), oriented[face_index][1])
    return oriented


def read_tet(path: pathlib.Path) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int, int]]]:
    lines = iter(path.read_text(encoding="utf-8").splitlines())
    vertex_header = next(lines).split()
    tet_header = next(lines).split()
    if len(vertex_header) != 2 or vertex_header[1] != "vertices" or len(tet_header) != 2 or tet_header[1] != "tets":
        raise ValueError("expected CDT inner-only ASCII .tet output")
    vertices = [tuple(float(value) for value in next(lines).split()) for _ in range(int(vertex_header[0]))]
    if any(len(vertex) != 3 for vertex in vertices):
        raise ValueError("malformed CDT vertex")
    tetrahedra: list[tuple[int, int, int, int]] = []
    for _ in range(int(tet_header[0])):
        fields = next(lines).split()
        if len(fields) != 5 or fields[0] != "4":
            raise ValueError("malformed CDT tetrahedron")
        tet = tuple(int(value) for value in fields[1:5])
        if any(index < 0 or index >= len(vertices) for index in tet):
            raise ValueError("CDT tetrahedron index outside point range")
        tetrahedra.append(tet)
    return vertices, tetrahedra


def canonicalize_output(
    source_vertices: list[tuple[float, float, float]],
    output_vertices: list[tuple[float, float, float]],
    tetrahedra: list[tuple[int, int, int, int]],
) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int, int]], float]:
    """Restore source IDs without masking a material boundary displacement.

    CDT reorders its input points.  A source point is restored only when one
    output point is uniquely within a scale-relative numerical-output tolerance.
    The returned maximum residual is reported so the caller can distinguish
    formatting loss from a changed boundary.
    """
    extent = max(
        max(coordinate[index] for coordinate in source_vertices) - min(coordinate[index] for coordinate in source_vertices)
        for index in range(3)
    )
    tolerance = max(1.0, extent) * 1.0e-12
    output_to_source: dict[int, int] = {}
    maximum_residual = 0.0
    for source_index, source in enumerate(source_vertices):
        candidates = sorted(
            ((sum((a - b) ** 2 for a, b in zip(source, point)), output_index) for output_index, point in enumerate(output_vertices)),
            key=lambda candidate: candidate[0],
        )
        if not candidates or candidates[0][0] > tolerance * tolerance:
            raise ValueError(f"source vertex {source_index} was not preserved within {tolerance:g}")
        if len(candidates) > 1 and candidates[1][0] <= tolerance * tolerance:
            raise ValueError(f"source vertex {source_index} has ambiguous output identity")
        distance, output_index = candidates[0]
        if output_index in output_to_source:
            raise ValueError("two source vertices map to one CDT output vertex")
        output_to_source[output_index] = source_index
        maximum_residual = max(maximum_residual, distance ** 0.5)
    reordered = list(source_vertices)
    output_to_canonical = dict(output_to_source)
    for output_index, point in enumerate(output_vertices):
        if output_index not in output_to_canonical:
            output_to_canonical[output_index] = len(reordered)
            reordered.append(point)
    return reordered, [tuple(output_to_canonical[index] for index in tet) for tet in tetrahedra], maximum_residual


def write_tetgen(prefix: pathlib.Path, vertices: list[tuple[float, float, float]], tetrahedra: list[tuple[int, int, int, int]]) -> None:
    with prefix.with_suffix(".1.node").open("w", encoding="utf-8") as output:
        output.write(f"{len(vertices)} 3 0 0\n")
        for index, (x, y, z) in enumerate(vertices):
            output.write(f"{index} {x:.17g} {y:.17g} {z:.17g}\n")
    with prefix.with_suffix(".1.ele").open("w", encoding="utf-8") as output:
        output.write(f"{len(tetrahedra)} 4 0\n")
        for index, tet in enumerate(tetrahedra):
            output.write(f"{index} {' '.join(str(value) for value in tet)}\n")


def subtract(a: tuple[float, float, float], b: tuple[float, float, float]) -> tuple[float, float, float]:
    return tuple(x - y for x, y in zip(a, b))


def dot(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
    return sum(x * y for x, y in zip(a, b))


def cross(a: tuple[float, float, float], b: tuple[float, float, float]) -> tuple[float, float, float]:
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def twice_area(a: tuple[float, float, float], b: tuple[float, float, float], c: tuple[float, float, float]) -> float:
    return dot(cross(subtract(b, a), subtract(c, a)), cross(subtract(b, a), subtract(c, a))) ** 0.5


def boundary_parent_audit(
    source_vertices: list[tuple[float, float, float]],
    facets: list[tuple[tuple[int, int, int], int]],
    output_vertices: list[tuple[float, float, float]],
    tetrahedra: list[tuple[int, int, int, int]],
) -> dict[str, object]:
    """Classify every output boundary triangle against one source parent.

    This is deliberately a geometric-facet *screen*, not a replacement for the
    project's exact rational coverage oracle: it proves no parent was silently
    moved and reports whether a backend subdivided it.  The existing complete
    verifier remains the literal-core acceptance gate.
    """
    face_uses: Counter[tuple[int, int, int]] = Counter()
    for tet in tetrahedra:
        for omitted in range(4):
            face_uses[tuple(sorted(tet[index] for index in range(4) if index != omitted))] += 1
    boundary = [face for face, uses in face_uses.items() if uses == 1]
    extent = max(
        max(point[axis] for point in source_vertices) - min(point[axis] for point in source_vertices)
        for axis in range(3)
    )
    tolerance = max(1.0, extent) * 1.0e-10
    parent_areas = [twice_area(*(source_vertices[index] for index in face)) * 0.5 for face, _ in facets]
    child_area = [0.0 for _ in facets]
    child_count = [0 for _ in facets]
    unassigned = 0
    ambiguous = 0
    unassigned_closest_marker: Counter[int] = Counter()
    maximum_nearest_parent_plane_distance = 0.0
    for face in boundary:
        points = [output_vertices[index] for index in face]
        candidates: list[int] = []
        for parent_index, ((a_index, b_index, c_index), _) in enumerate(facets):
            a, b, c = source_vertices[a_index], source_vertices[b_index], source_vertices[c_index]
            normal = cross(subtract(b, a), subtract(c, a))
            normal_length = dot(normal, normal) ** 0.5
            if normal_length == 0.0:
                continue
            valid = True
            for point in points:
                if abs(dot(normal, subtract(point, a))) > tolerance * normal_length:
                    valid = False
                    break
                u = dot(cross(subtract(b, point), subtract(c, point)), normal) / (normal_length * normal_length)
                v = dot(cross(subtract(c, point), subtract(a, point)), normal) / (normal_length * normal_length)
                w = 1.0 - u - v
                if min(u, v, w) < -tolerance:
                    valid = False
                    break
            if valid:
                candidates.append(parent_index)
        if len(candidates) != 1:
            unassigned += len(candidates) == 0
            ambiguous += len(candidates) > 1
            nearest_distance = float("inf")
            nearest_marker = -1
            for (a_index, b_index, c_index), marker in facets:
                a, b, c = source_vertices[a_index], source_vertices[b_index], source_vertices[c_index]
                normal = cross(subtract(b, a), subtract(c, a))
                normal_length = dot(normal, normal) ** 0.5
                if normal_length == 0.0:
                    continue
                distance = max(abs(dot(normal, subtract(point, a))) / normal_length for point in points)
                if distance < nearest_distance:
                    nearest_distance, nearest_marker = distance, marker
            if nearest_marker >= 0:
                unassigned_closest_marker[nearest_marker] += 1
                maximum_nearest_parent_plane_distance = max(maximum_nearest_parent_plane_distance, nearest_distance)
            continue
        parent_index = candidates[0]
        child_count[parent_index] += 1
        child_area[parent_index] += twice_area(*points) * 0.5
    exact_literal = sum(
        1 for face, _ in facets if tuple(sorted(face)) in face_uses and face_uses[tuple(sorted(face))] == 1
    )
    by_marker: dict[int, dict[str, int]] = {}
    area_complete = 0
    for index, (_, marker) in enumerate(facets):
        result = by_marker.setdefault(marker, {"parents": 0, "area_complete": 0, "subdivided": 0})
        result["parents"] += 1
        complete = abs(child_area[index] - parent_areas[index]) <= tolerance * max(1.0, parent_areas[index])
        area_complete += complete
        result["area_complete"] += complete
        result["subdivided"] += child_count[index] > 1
    return {
        "output_boundary_faces": len(boundary),
        "literal_source_faces_on_output_boundary": exact_literal,
        "source_parents_area_complete": area_complete,
        "source_parent_count": len(facets),
        "unassigned_output_boundary_faces": unassigned,
        "ambiguous_output_boundary_faces": ambiguous,
        "unassigned_nearest_parent_marker_counts": dict(unassigned_closest_marker),
        "maximum_nearest_parent_plane_distance": maximum_nearest_parent_plane_distance,
        "by_marker": by_marker,
    }


def boundary_faces(tetrahedra: list[tuple[int, int, int, int]]) -> list[tuple[int, int, int]]:
    """Return the unoriented skin of a tetrahedron set."""
    uses: Counter[tuple[int, int, int]] = Counter()
    for tet in tetrahedra:
        for omitted in range(4):
            uses[tuple(sorted(tet[index] for index in range(4) if index != omitted))] += 1
    return [face for face, count in uses.items() if count == 1]


def face_in_parent(points: list[tuple[float, float, float]], parent: tuple[int, int, int], vertices: list[tuple[float, float, float]], tolerance: float) -> bool:
    a, b, c = (vertices[index] for index in parent)
    normal = cross(subtract(b, a), subtract(c, a))
    normal_squared = dot(normal, normal)
    if normal_squared == 0.0:
        return False
    for point in points:
        if abs(dot(normal, subtract(point, a))) > tolerance * normal_squared ** 0.5:
            return False
        u = dot(cross(subtract(b, point), subtract(c, point)), normal) / normal_squared
        v = dot(cross(subtract(c, point), subtract(a, point)), normal) / normal_squared
        if min(u, v, 1.0 - u - v) < -tolerance:
            return False
    return True


def core_interface_handshake_audit(
    poly_path: pathlib.Path,
    source_vertices: list[tuple[float, float, float]],
    facets: list[tuple[tuple[int, int, int], int]],
    output_vertices: list[tuple[float, float, float]],
    tetrahedra: list[tuple[int, int, int, int]],
) -> dict[str, object]:
    """Describe whether CDT's core skin can attach to the unchanged core.

    A generated point in the interior of an exposed regular-core face cannot
    attach to the existing, unrefined core: its face has no matching vertex.
    This audit intentionally makes that incompatibility measurable rather than
    attempting a float-based repair.
    """
    core_path = pathlib.Path(f"{poly_path}.core")
    if not core_path.is_file():
        return {"available": False, "reason": "missing .poly.core sidecar"}
    lines = iter(core_path.read_text(encoding="utf-8").splitlines())
    vertex_count, tet_count = (int(value) for value in next(lines).split())
    core_vertices: dict[int, tuple[int, tuple[float, float, float]]] = {}
    for _ in range(vertex_count):
        fields = next(lines).split()
        core_vertices[int(fields[0])] = (int(fields[1]), tuple(float(value) for value in fields[2:5]))
    core_tets = [tuple(int(value) for value in next(lines).split()) for _ in range(tet_count)]
    face_uses: Counter[tuple[int, int, int]] = Counter()
    for tet in core_tets:
        for omitted in range(4):
            face_uses[tuple(sorted(tet[index] for index in range(4) if index != omitted))] += 1
    exposed = [face for face, count in face_uses.items() if count == 1]
    exposed_points = [tuple(core_vertices[identifier][1] for identifier in face) for face in exposed]
    source_core_indices = {source_index for source_index, _ in core_vertices.values() if source_index >= 0}
    extent = max(max(point[axis] for point in source_vertices) - min(point[axis] for point in source_vertices) for axis in range(3))
    tolerance = max(1.0, extent) * 1.0e-10
    classifications: Counter[str] = Counter()
    refinement_patterns: Counter[str] = Counter()
    child_count_histogram: Counter[str] = Counter()
    edge_midpoint_conforming_parents = 0
    maximum_midpoint_parameter_residual = 0.0
    core_faces = 0
    nonliteral_core_faces = 0
    seen_vertices: set[int] = set()
    children_by_parent: dict[int, list[tuple[int, int, int]]] = {}
    for face in boundary_faces(tetrahedra):
        points = [output_vertices[index] for index in face]
        # A core child is completely coplanar and contained in one marker-3
        # parent.  Parent triangles are disjoint except at their edges, so this
        # is deliberately conservative at shared parent edges.
        parent_index = None
        for candidate_index, (parent, marker) in enumerate(facets):
            if marker != 3:
                continue
            if face_in_parent(points, parent, source_vertices, tolerance):
                parent_index = candidate_index
                break
        if parent_index is None:
            continue
        core_faces += 1
        if any(index >= len(source_vertices) for index in face):
            nonliteral_core_faces += 1
        seen_vertices.update(face)
        children_by_parent.setdefault(parent_index, []).append(face)
    for index in seen_vertices:
        point = output_vertices[index]
        if index < len(source_vertices) and index in source_core_indices:
            classifications["existing_core_vertex"] += 1
            continue
        edge_parameters: list[float] = []
        for triangle in exposed_points:
            for a, b in ((0, 1), (1, 2), (2, 0)):
                start, end = triangle[a], triangle[b]
                direction = subtract(end, start)
                length_squared = dot(direction, direction)
                if length_squared == 0.0:
                    continue
                parameter = dot(subtract(point, start), direction) / length_squared
                closest = tuple(start[axis] + parameter * direction[axis] for axis in range(3))
                if -tolerance <= parameter <= 1.0 + tolerance and dot(subtract(point, closest), subtract(point, closest)) <= tolerance * tolerance:
                    edge_parameters.append(parameter)
        if edge_parameters:
            classifications["new_or_noncore_vertex_on_core_edge"] += 1
            maximum_midpoint_parameter_residual = max(maximum_midpoint_parameter_residual, min(abs(parameter - 0.5) for parameter in edge_parameters))
        else:
            classifications["new_or_noncore_vertex_in_core_face"] += 1
    # Recognize only the exact triangular red-face language.  This is a
    # compatibility check for the present core grammar, not a claim that red
    # refinement is the only possible future handshake.
    for parent_index, children in children_by_parent.items():
        parent = facets[parent_index][0]
        local_labels: dict[int, str] = {}
        for child in children:
            for index in child:
                point = output_vertices[index]
                a, b, c = (source_vertices[vertex] for vertex in parent)
                normal = cross(subtract(b, a), subtract(c, a))
                denominator = dot(normal, normal)
                u = dot(cross(subtract(b, point), subtract(c, point)), normal) / denominator
                v = dot(cross(subtract(c, point), subtract(a, point)), normal) / denominator
                weights = (u, v, 1.0 - u - v)
                choices = (("a", (1.0, 0.0, 0.0)), ("b", (0.0, 1.0, 0.0)), ("c", (0.0, 0.0, 1.0)), ("ab", (0.5, 0.5, 0.0)), ("bc", (0.0, 0.5, 0.5)), ("ca", (0.5, 0.0, 0.5)))
                label = next((name for name, expected in choices if max(abs(x - y) for x, y in zip(weights, expected)) <= tolerance), "other")
                local_labels[index] = label
        child_sets = {frozenset(local_labels[index] for index in child) for child in children}
        child_count_histogram[str(len(children))] += 1
        red_sets = {frozenset(labels) for labels in (("a", "ab", "ca"), ("ab", "b", "bc"), ("ca", "bc", "c"), ("ab", "bc", "ca"))}
        if len(children) == 1 and child_sets == {frozenset(("a", "b", "c"))}:
            refinement_patterns["literal"] += 1
        elif len(children) == 4 and child_sets == red_sets:
            refinement_patterns["red_face"] += 1
        else:
            if all(label != "other" for label in local_labels.values()):
                refinement_patterns["midpoint_language_nonred"] += 1
            else:
                refinement_patterns["other"] += 1
        midpoint_count = sum(label in {"ab", "bc", "ca"} for label in local_labels.values())
        if all(label != "other" for label in local_labels.values()) and len(children) == midpoint_count + 1:
            edge_midpoint_conforming_parents += 1
    incompatible = classifications["new_or_noncore_vertex_on_core_edge"] + classifications["new_or_noncore_vertex_in_core_face"]
    return {
        "available": True,
        "core_output_boundary_faces": core_faces,
        "nonliteral_core_output_boundary_faces": nonliteral_core_faces,
        "core_skin_vertex_classes": dict(classifications),
        "core_parent_refinement_patterns": dict(refinement_patterns),
        "core_parent_child_count_histogram": dict(child_count_histogram),
        "edge_midpoint_conforming_core_parents": edge_midpoint_conforming_parents,
        "max_core_edge_midpoint_parameter_residual": maximum_midpoint_parameter_residual,
        "unchanged_core_attachment": incompatible == 0,
        "reason": "CDT introduced no new core-skin vertices" if incompatible == 0 else "CDT core skin has vertices absent from the unchanged core; a refinement handshake is required",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("poly", type=pathlib.Path)
    parser.add_argument("--cdt", required=True, type=pathlib.Path)
    parser.add_argument("--prefix", required=True, type=pathlib.Path)
    parser.add_argument("--json", type=pathlib.Path, help="write the machine-readable result in addition to stdout")
    parser.add_argument("--permute-seed", type=int, help="independently permute OFF vertex and facet records before invoking CDT")
    args = parser.parse_args()
    vertices, facets = read_poly(args.poly)
    facets = orient_closed_components(vertices, facets)
    off_path = args.prefix.with_suffix(".off")
    vertex_order = facet_order = None
    if args.permute_seed is not None:
        generator = random.Random(args.permute_seed)
        vertex_order, facet_order = list(range(len(vertices))), list(range(len(facets)))
        generator.shuffle(vertex_order)
        generator.shuffle(facet_order)
    write_off(off_path, vertices, facets, vertex_order, facet_order)
    result = subprocess.run([str(args.cdt), "-r", str(off_path)], check=False)
    if result.returncode:
        return result.returncode
    tet_path = pathlib.Path(f"{off_path}.tet")
    if not tet_path.is_file():
        raise ValueError("CDT did not produce inner-only .tet output")
    output_vertices, tetrahedra = read_tet(tet_path)
    output_vertices, tetrahedra, max_source_residual = canonicalize_output(vertices, output_vertices, tetrahedra)
    audit = boundary_parent_audit(vertices, facets, output_vertices, tetrahedra)
    handshake = core_interface_handshake_audit(args.poly, vertices, facets, output_vertices, tetrahedra)
    write_tetgen(args.prefix, output_vertices, tetrahedra)
    if args.prefix.with_suffix(".poly") != args.poly:
        shutil.copyfile(args.poly, args.prefix.with_suffix(".poly"))
        source_core = pathlib.Path(f"{args.poly}.core")
        if source_core.is_file():
            shutil.copyfile(source_core, pathlib.Path(f"{args.prefix.with_suffix('.poly')}.core"))
    report = {
        "input_vertices": len(vertices),
        "input_facets": len(facets),
        "output_vertices": len(output_vertices),
        "output_tetrahedra": len(tetrahedra),
        "max_source_vertex_residual": max_source_residual,
        "off_permutation_seed": args.permute_seed,
        "boundary_parent_audit": audit,
        "core_interface_handshake_audit": handshake,
    }
    serialized = json.dumps(report, sort_keys=True)
    if args.json:
        args.json.write_text(serialized + "\n", encoding="utf-8")
    print(serialized)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, StopIteration) as error:
        print(f"diazzi baseline adapter: {error}", file=sys.stderr)
        raise SystemExit(2)
