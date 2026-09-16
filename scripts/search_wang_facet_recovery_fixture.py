#!/usr/bin/env python3
"""Find a closed surface whose author run enters the facet-recovery stage."""
import argparse
import pathlib
import random
import re
import subprocess
import tempfile

FACES = ((0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7),
         (0, 1, 5), (0, 5, 4), (3, 7, 6), (3, 6, 2),
         (0, 4, 7), (0, 7, 3), (1, 2, 6), (1, 6, 5))
BASE = ((-1., -1., -1.), (1., -1., -1.), (1., 1., -1.),
        (-1., 1., -1.), (-1., -1., 1.), (1., -1., 1.),
        (1., 1., 1.), (-1., 1., 1.))
OCTAHEDRON_FACES = ((0, 2, 4), (2, 1, 4), (1, 3, 4), (3, 0, 4),
                    (2, 0, 5), (1, 2, 5), (3, 1, 5), (0, 3, 5))
OCTAHEDRON_BASE = ((1., 0., 0.), (-1., 0., 0.), (0., 1., 0.),
                   (0., -1., 0.), (0., 0., 1.), (0., 0., -1.))
# A cube whose upper face is replaced with an inward four-triangle cap.  The
# cap is a valid closed concavity for 0 < cap_z < 1, unlike arbitrary vertex
# perturbation where the prescribed surface can self-intersect.
DENT_FACES = ((0, 2, 1), (0, 3, 2),
              (0, 1, 5), (0, 5, 4), (3, 7, 6), (3, 6, 2),
              (0, 4, 7), (0, 7, 3), (1, 2, 6), (1, 6, 5),
              (4, 5, 8), (5, 6, 8), (6, 7, 8), (7, 4, 8))

def write_surface(path, points, faces):
    text = ["# vtk DataFile Version 3.0", "Wang facet recovery fixture",
            "ASCII", "DATASET POLYDATA", f"POINTS {len(points)} double"]
    text += [f"{x:.17g} {y:.17g} {z:.17g}" for x, y, z in points]
    text += [f"POLYGONS {len(faces)} {len(faces) * 4}"]
    text += [f"3 {a} {b} {c}" for a, b, c in faces]
    path.write_text("\n".join(text) + "\n", encoding="ascii")

def field(path, name):
    for line in path.read_text(encoding="ascii").splitlines():
        if line.startswith(name + " "):
            return int(line.split()[1])
    return -1

def convex_octahedron(points):
    """Reject any candidate whose fixed octahedron faces are not a convex hull."""
    for a, b, c in OCTAHEDRON_FACES:
        ax, ay, az = points[a]
        bx, by, bz = points[b]
        cx, cy, cz = points[c]
        ux, uy, uz = bx-ax, by-ay, bz-az
        vx, vy, vz = cx-ax, cy-ay, cz-az
        nx, ny, nz = (uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx)
        for index, (x, y, z) in enumerate(points):
            if index not in (a, b, c) and nx*(x-ax)+ny*(y-ay)+nz*(z-az) >= -1e-7:
                return False
    return True

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--attempts", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=20260915)
    parser.add_argument("--family", choices=("cube", "octahedron", "dented"),
                        default="dented")
    parser.add_argument("--output", type=pathlib.Path,
                        default=pathlib.Path("tests/fixtures/wang/reference_facet_recovery_surface.vtk"))
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    author = root / "build-wang-reference/wang_author_reference_probe"
    rng = random.Random(args.seed)
    with tempfile.TemporaryDirectory(prefix="wang-facet-search-") as temp:
        surface = pathlib.Path(temp) / "candidate.vtk"
        trace = pathlib.Path(temp) / "trace.txt"
        for attempt in range(args.attempts):
            if args.family == "cube":
                points = []
                for x, y, z in BASE:
                    scale = rng.uniform(.4, 2.5)
                    points.append((scale*x + rng.uniform(-.7, .7),
                                   scale*y + rng.uniform(-.7, .7),
                                   scale*z + rng.uniform(-.7, .7)))
                faces = FACES
            elif args.family == "octahedron":
                points = []
                for x, y, z in OCTAHEDRON_BASE:
                    scale = rng.uniform(.35, 3.0)
                    points.append((scale*x + rng.uniform(-.15, .15),
                                   scale*y + rng.uniform(-.15, .15),
                                   scale*z + rng.uniform(-.15, .15)))
                if not convex_octahedron(points):
                    continue
                faces = OCTAHEDRON_FACES
            else:
                sx, sy = rng.uniform(.4, 3.0), rng.uniform(.4, 3.0)
                bottom, top = -rng.uniform(.4, 3.0), rng.uniform(.4, 3.0)
                points = [(-sx, -sy, bottom), (sx, -sy, bottom),
                          (sx, sy, bottom), (-sx, sy, bottom),
                          (-sx, -sy, top), (sx, -sy, top),
                          (sx, sy, top), (-sx, sy, top),
                          (rng.uniform(-.35*sx, .35*sx),
                           rng.uniform(-.35*sy, .35*sy),
                           top-rng.uniform(.05*(top-bottom),
                                           .95*(top-bottom)))]
                faces = DENT_FACES
            write_surface(surface, points, faces)
            run = subprocess.run([author, "cube", str(surface), str(trace)],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True)
            if run.returncode or field(trace, "segment_missing_edges") != 0:
                continue
            if field(trace, "segment_missing_facets") <= 0:
                continue
            # A nonempty level-1000 round is the author pass immediately
            # before recoverFaces(..., 1), which owns addinSt.
            if not re.search(r"level:1000 lost faces:[1-9]", run.stdout):
                continue
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(surface.read_bytes())
            print(f"FOUND attempt={attempt} facets={field(trace, 'segment_missing_facets')}")
            return 0
    print("NONE")
    return 1

if __name__ == "__main__":
    raise SystemExit(main())
