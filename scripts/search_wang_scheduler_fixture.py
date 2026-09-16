#!/usr/bin/env python3
"""Deterministically search for an R3 same-seed scheduler fixture."""

import argparse
import pathlib
import random
import subprocess
import tempfile


FACES = (
    (4, 0, 2), (4, 2, 1), (4, 1, 3), (4, 3, 0),
    (5, 2, 0), (5, 1, 2), (5, 3, 1), (5, 0, 3),
)
BASE = (
    (1.0, 0.0, 0.0), (-1.0, 0.0, 0.0),
    (0.0, 1.0, 0.0), (0.0, -1.0, 0.0),
    (0.0, 0.0, 1.0), (0.0, 0.0, -1.0),
)


def write_vtk(path: pathlib.Path, points) -> None:
    lines = [
        "# vtk DataFile Version 3.0", "Wang R3 scheduler search", "ASCII",
        "DATASET POLYDATA", f"POINTS {len(points)} double",
    ]
    lines.extend(f"{x:.17g} {y:.17g} {z:.17g}" for x, y, z in points)
    lines.append(f"POLYGONS {len(FACES)} {len(FACES) * 4}")
    lines.extend(f"3 {a} {b} {c}" for a, b, c in FACES)
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def fields(path: pathlib.Path, prefix: str):
    return [line for line in path.read_text(encoding="ascii").splitlines()
            if line.startswith(prefix)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--attempts", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=20260914)
    parser.add_argument("--output", type=pathlib.Path,
                        default=pathlib.Path("/tmp/wang_scheduler_candidate.vtk"))
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    author = root / "build-wang-reference/wang_author_reference_probe"
    prototype = root / "build-release/wang_prototype_reference_probe"
    rng = random.Random(args.seed)
    qualified = seed_matches = 0
    with tempfile.TemporaryDirectory(prefix="wang-r3-search-") as directory:
        scratch = pathlib.Path(directory)
        vtk = scratch / "candidate.vtk"
        author_trace = scratch / "author.txt"
        prototype_trace = scratch / "prototype.txt"
        for attempt in range(args.attempts):
            scale = rng.uniform(0.7, 2.0)
            points = [tuple(scale * value + rng.uniform(-0.45, 0.45)
                            for value in point) for point in BASE]
            write_vtk(vtk, points)
            author_run = subprocess.run(
                [author, "scheduler", vtk, author_trace],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            prototype_run = subprocess.run(
                [prototype, "scheduler_file", vtk, prototype_trace],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if author_run.returncode or prototype_run.returncode:
                continue
            author_events = fields(author_trace, "scheduler ")
            initial = [event for event in author_events
                       if event.split()[3] == "1" and event.split()[-1] == "0"]
            splits = [event for event in author_events
                      if event.split()[7:9] == ["2", "2"]]
            if len(initial) < 2 or len(splits) != 1:
                continue
            qualified += 1
            if fields(author_trace, "seed ") != fields(prototype_trace, "seed "):
                continue
            seed_matches += 1
            args.output.write_bytes(vtk.read_bytes())
            args.output.with_suffix(".author.txt").write_bytes(author_trace.read_bytes())
            args.output.with_suffix(".prototype.txt").write_bytes(
                prototype_trace.read_bytes())
            print(f"CANDIDATE {attempt} initial={len(initial)}")
            return 0
    print(f"NONE qualified={qualified} seed_matches={seed_matches}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
