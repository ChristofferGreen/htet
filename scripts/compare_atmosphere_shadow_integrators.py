#!/usr/bin/env python3
"""Compare direct-shadow-loss captures with a dense integration oracle."""

import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def load_rgb(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path), dtype=np.float64) / 255.0


def load_mask(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path), dtype=np.uint8) > 0


def row_boundaries(values: np.ndarray, threshold: float) -> list[np.ndarray]:
    active = values >= threshold
    return [np.flatnonzero(row[1:] != row[:-1]) + 0.5 for row in active]


def boundary_distance(candidate: np.ndarray, reference: np.ndarray,
                      threshold: float) -> float:
    candidate_rows = row_boundaries(candidate, threshold)
    reference_rows = row_boundaries(reference, threshold)
    distances: list[float] = []
    for candidate_points, reference_points in zip(candidate_rows,
                                                   reference_rows):
        if not len(reference_points):
            continue
        if not len(candidate_points):
            distances.extend([float(candidate.shape[1])] * len(reference_points))
            continue
        distances.extend(float(np.min(np.abs(candidate_points - point)))
                         for point in reference_points)
    return float(np.mean(distances)) if distances else 0.0


def compare(candidate_dir: Path, oracle_dir: Path) -> dict[str, object]:
    def stem(directory: Path) -> str:
        return ("direct-shadow-loss" if
                (directory / "direct-shadow-loss.ppm").exists() else
                "direct-loss")
    candidate_stem = stem(candidate_dir)
    oracle_stem = stem(oracle_dir)
    candidate = load_rgb(candidate_dir / f"{candidate_stem}.ppm")
    reference = load_rgb(oracle_dir / f"{oracle_stem}.ppm")
    mask = (load_mask(candidate_dir / f"{candidate_stem}.clear.pgm") &
            load_mask(oracle_dir / f"{oracle_stem}.clear.pgm"))
    difference = candidate - reference
    selected = difference[mask]
    luminance = np.array([0.2126, 0.7152, 0.0722])
    candidate_luminance = candidate @ luminance
    reference_luminance = reference @ luminance
    outside_reference = mask & (reference_luminance < 1.0 / 255.0)
    outside_excess = np.maximum(
        candidate_luminance-reference_luminance, 0.0)[outside_reference]
    gradient_error_x = (np.diff(candidate_luminance, axis=1) -
                        np.diff(reference_luminance, axis=1))
    gradient_error_y = (np.diff(candidate_luminance, axis=0) -
                        np.diff(reference_luminance, axis=0))
    gradient_mask_x = mask[:, 1:] & mask[:, :-1]
    gradient_mask_y = mask[1:, :] & mask[:-1, :]
    gradient_energy = (float(np.mean(gradient_error_x[gradient_mask_x] ** 2)) +
                       float(np.mean(gradient_error_y[gradient_mask_y] ** 2)))
    return {
        "candidate": candidate_dir.name,
        "oracle": oracle_dir.name,
        "masked_pixels": int(mask.sum()),
        "normalized_rgb_rmse": float(np.sqrt(np.mean(selected ** 2))),
        "maximum_rgb_error": float(np.max(np.abs(selected))),
        "shadow_boundary_distance_pixels": boundary_distance(
            candidate_luminance * mask, reference_luminance * mask, 4.0 / 255.0),
        "gradient_step_energy": gradient_energy,
        "outside_reference_mean_excess": (float(np.mean(outside_excess))
                                            if outside_excess.size else 0.0),
        "outside_reference_maximum_excess": (float(np.max(outside_excess))
                                               if outside_excess.size else 0.0),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("oracle", type=Path)
    parser.add_argument("candidates", nargs="+", type=Path)
    args = parser.parse_args()
    for candidate in args.candidates:
        print(json.dumps({"event": "atmosphere_shadow_integrator_comparison",
                          **compare(candidate, args.oracle)}, sort_keys=True))


if __name__ == "__main__":
    main()
