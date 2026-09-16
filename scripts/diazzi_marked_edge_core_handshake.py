#!/usr/bin/env python3
"""Test a deterministic marked-edge handshake between Diazzi CDT and the core.

This is an offline viability probe.  It does not promote CDT, the coning
refinement used here, or any Python code to the runtime path.  Its narrow
claim is stronger: it either proves that the CDT core skin can be reproduced
exactly by a conformingly refined regular-core volume, or names the first
unsupported skin feature.
"""
from __future__ import annotations

import argparse
import json
import math
import pathlib
from collections import Counter, defaultdict

from diazzi_cdt_complete_volume_baseline import (
    boundary_faces, canonicalize_output, cross, dot, face_in_parent, read_poly,
    read_tet, subtract,
)


def six(a, b, c, d):
    return dot(subtract(b, a), cross(subtract(c, a), subtract(d, a)))


def canonical_face(face):
    # Stable IDs intentionally mix source integer IDs and tagged generated
    # IDs; repr gives a deterministic total ordering without float authority.
    return tuple(sorted(face, key=repr))


def core_sidecar(path):
    lines = iter(path.read_text(encoding="utf-8").splitlines())
    vertex_count, tet_count = (int(value) for value in next(lines).split())
    vertices = {}
    for _ in range(vertex_count):
        fields = next(lines).split()
        vertices[int(fields[0])] = (int(fields[1]), tuple(float(value) for value in fields[2:5]))
    return vertices, [tuple(int(value) for value in next(lines).split()) for _ in range(tet_count)]


def face_children(face, marked):
    """Triangulate a triangular face solely from its globally marked edges."""
    a, b, c = face
    midpoint = lambda x, y: ("m",) + tuple(sorted((x, y)))
    ab, bc, ca = midpoint(a, b), midpoint(b, c), midpoint(c, a)
    has = (ab in marked, bc in marked, ca in marked)
    if has == (False, False, False):
        return [(a, b, c)]
    if has == (True, False, False):
        return [(a, ab, c), (ab, b, c)]
    if has == (False, True, False):
        return [(b, bc, a), (bc, c, a)]
    if has == (False, False, True):
        return [(c, ca, b), (ca, a, b)]
    if has == (True, True, False):
        return [(a, ab, c), (ab, bc, c), (ab, b, bc)]
    if has == (False, True, True):
        return [(b, bc, a), (bc, ca, a), (bc, c, ca)]
    if has == (True, False, True):
        return [(c, ca, b), (ca, ab, b), (ca, a, ab)]
    return [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]


def dihedral_range(points):
    values = []
    normals = []
    for omitted in range(4):
        face = [index for index in range(4) if index != omitted]
        normal = cross(subtract(points[face[1]], points[face[0]]), subtract(points[face[2]], points[face[0]]))
        if dot(normal, subtract(points[omitted], points[face[0]])) > 0.0:
            normal = tuple(-value for value in normal)
        normals.append(normal)
    for first in range(4):
        for second in range(first + 1, 4):
            scale = math.sqrt(dot(normals[first], normals[first]) * dot(normals[second], normals[second]))
            if scale == 0.0:
                return 0.0, 180.0
            values.append(math.degrees(math.pi - math.acos(max(-1.0, min(1.0, dot(normals[first], normals[second]) / scale)))))
    return min(values), max(values)


def quality_summary(tetrahedra, lookup):
    minimum_ratio, minimum_dihedral, maximum_dihedral, maximum_edge_ratio = float("inf"), 180.0, 0.0, 0.0
    below_ratio = below_dihedral = above_dihedral = 0
    for tet in tetrahedra:
        p = [lookup(vertex) for vertex in tet]
        lengths = [math.dist(p[a], p[b]) for a, b in ((0, 1), (0, 2), (0, 3), (1, 2), (1, 3), (2, 3))]
        volume6 = abs(six(*p))
        ratio = 12.0 * (volume6 / 2.0) ** (2.0 / 3.0) / sum(length * length for length in lengths)
        low, high = dihedral_range(p)
        minimum_ratio, minimum_dihedral = min(minimum_ratio, ratio), min(minimum_dihedral, low)
        maximum_dihedral, maximum_edge_ratio = max(maximum_dihedral, high), max(maximum_edge_ratio, max(lengths) / min(lengths))
        below_ratio += ratio < .01
        below_dihedral += sum(value < 5.0 for value in (low,))
        above_dihedral += sum(value > 175.0 for value in (high,))
    return {"min_mean_ratio": minimum_ratio, "min_dihedral": minimum_dihedral, "max_dihedral": maximum_dihedral, "max_edge_ratio": maximum_edge_ratio, "elements_below_mean_ratio_01": below_ratio, "tetrahedra_with_min_dihedral_below_5": below_dihedral, "tetrahedra_with_max_dihedral_above_175": above_dihedral, "qualified": minimum_ratio >= .01 and minimum_dihedral >= 5.0 and maximum_dihedral <= 175.0 and maximum_edge_ratio <= 20.0}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("poly", type=pathlib.Path, help="source complete-volume PLC")
    parser.add_argument("--tet", required=True, type=pathlib.Path, help="Diazzi .off.tet output")
    args = parser.parse_args()
    source, facets = read_poly(args.poly)
    output, shell = read_tet(args.tet)
    output, shell, _ = canonicalize_output(source, output, shell)
    core_vertices, core_tets = core_sidecar(pathlib.Path(f"{args.poly}.core"))
    source_to_core = {source_id: core_id for core_id, (source_id, _) in core_vertices.items() if source_id >= 0}
    extent = max(max(point[axis] for point in source) - min(point[axis] for point in source) for axis in range(3))
    tolerance = max(1.0, extent) * 1.0e-10
    # Identify each output core-skin vertex by a stable source core vertex or
    # by the stable ID of the regular-core edge whose midpoint it occupies.
    core_face_uses = Counter(canonical_face(tet[index] for index in range(4) if index != omitted) for tet in core_tets for omitted in range(4))
    core_edges = {tuple(sorted((face[i], face[j]))) for face, uses in core_face_uses.items() if uses == 1 for i, j in ((0, 1), (1, 2), (2, 0))}
    midpoint_points = {edge: tuple((core_vertices[edge[0]][1][axis] + core_vertices[edge[1]][1][axis]) * .5 for axis in range(3)) for edge in core_edges}
    output_id = {}
    marked = set()
    unsupported = []
    for index, point in enumerate(output):
        if index in source_to_core:
            output_id[index] = source_to_core[index]
            continue
        matches = [edge for edge, midpoint in midpoint_points.items() if math.dist(point, midpoint) <= tolerance]
        if len(matches) == 1:
            edge = matches[0]
            output_id[index] = ("m",) + edge
            marked.add(output_id[index])
        elif matches:
            unsupported.append({"output_vertex": index, "reason": "ambiguous_core_edge_midpoint"})
    # Map CDT core boundary triangles to stable core/midpoint IDs.
    cdt_skin = set()
    cdt_children_by_parent = defaultdict(list)
    for face in boundary_faces(shell):
        points = [output[index] for index in face]
        parents = [parent for parent, marker in facets if marker == 3 and face_in_parent(points, parent, source, tolerance)]
        if not parents:
            continue
        if len(parents) != 1 or any(index not in output_id for index in face):
            unsupported.append({"output_face": list(face), "reason": "unmappable_or_ambiguous_core_child"})
            continue
        child = canonical_face(output_id[index] for index in face)
        cdt_skin.add(child)
        parent_core_face = canonical_face(source_to_core[index] for index in parents[0])
        cdt_children_by_parent[parent_core_face].append(child)
    # Every marked edge refines every incident core parent.  Cone each
    # refined parent's compatible boundary triangulation to a private centroid;
    # leave all-unmarked parents intact.  Shared faces are generated only from
    # stable edge marks, so equality across parents is structural, not float
    # inferred.
    points = {identifier: position for identifier, (_, position) in core_vertices.items()}
    points.update({midpoint: midpoint_points[edge] for edge in core_edges for midpoint in [("m",) + edge] if midpoint in marked})
    refined = []
    generated = []
    for parent_index, parent in enumerate(core_tets):
        parent_marks = {edge for edge in marked if edge[1] in parent and edge[2] in parent}
        if not parent_marks:
            generated.append(parent)
            continue
        refined.append(parent_index)
        centroid = ("c", parent_index)
        points[centroid] = tuple(sum(points[vertex][axis] for vertex in parent) * .25 for axis in range(3))
        for omitted in range(4):
            face = tuple(parent[index] for index in range(4) if index != omitted)
            # A two-edge midpoint face has two valid internal diagonals.  The
            # backend's recovered skin is therefore authoritative for an
            # exposed core face; stable marked-edge grammar remains sufficient
            # for every core-internal face.
            children = cdt_children_by_parent.get(canonical_face(face), face_children(face, marked))
            for child in children:
                tet = (centroid,) + child
                if six(*(points[vertex] for vertex in tet)) < 0.0:
                    tet = (tet[0], tet[2], tet[1], tet[3])
                generated.append(tet)
    generated_uses = Counter(canonical_face(tet[index] for index in range(4) if index != omitted) for tet in generated for omitted in range(4))
    generated_skin = {face for face, uses in generated_uses.items() if uses == 1}
    expected_skin = {face for face in generated_skin if all(isinstance(vertex, int) or vertex[0] == "m" for vertex in face)}
    # The selected core has no other boundary category; this equality is the
    # handshake itself.
    nonpositive = sum(six(*(points[vertex] for vertex in tet)) <= 1e-13 for tet in generated)
    ranges = [dihedral_range([points[vertex] for vertex in tet]) for tet in generated]
    # Shell point IDs already use output indices.  Map the core's stable IDs
    # onto those exact indices before evaluating the assembled candidate.
    stable_to_output = {core_id: source_id for source_id, core_id in source_to_core.items()}
    stable_to_output.update({stable: index for index, stable in output_id.items()})
    next_output = len(output)
    combined_points = list(output)
    for stable, point in points.items():
        if stable not in stable_to_output:
            stable_to_output[stable] = next_output
            combined_points.append(point)
            next_output += 1
    combined_core = [tuple(stable_to_output[vertex] for vertex in tet) for tet in generated]
    combined = shell + combined_core
    combined_uses = Counter(canonical_face(tet[index] for index in range(4) if index != omitted) for tet in combined for omitted in range(4))
    interface_uses = [combined_uses[canonical_face(stable_to_output[vertex] for vertex in face)] for face in cdt_skin]
    report = {
        "status": "accepted" if not unsupported and cdt_skin == expected_skin and not nonpositive else "refused",
        "unsupported": unsupported,
        "cdt_core_skin_faces": len(cdt_skin),
        "refined_core_skin_faces": len(expected_skin),
        "skin_missing_from_refined_core": len(cdt_skin - expected_skin),
        "unexpected_refined_core_skin_faces": len(expected_skin - cdt_skin),
        "marked_core_edges": len(marked),
        "refined_core_parents": len(refined),
        "total_core_parents": len(core_tets),
        "generated_core_tetrahedra": len(generated),
        "nonpositive_generated_core_tetrahedra": nonpositive,
        "core_min_dihedral": min(value[0] for value in ranges),
        "core_max_dihedral": max(value[1] for value in ranges),
        "combined_topology": {"nonmanifold_faces": sum(uses > 2 for uses in combined_uses.values()), "interface_faces_not_twice_used": sum(uses != 2 for uses in interface_uses)},
        "combined_s4_diagnostic": quality_summary(combined, lambda index: combined_points[index]),
    }
    print(json.dumps(report, sort_keys=True))
    return 0 if report["status"] == "accepted" else 1


if __name__ == "__main__":
    raise SystemExit(main())
