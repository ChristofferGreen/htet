#!/usr/bin/env bash
set -euo pipefail

# Qualify the Hillaire 200x100 sky-view lookup against the production 384x216
# resource on the native reference-temporal Metal renderer.  This deliberately
# drives --metal-atmosphere-capture rather than the Vulkan capture utility: a
# passing matrix therefore proves the exact executable and resource override
# considered for promotion.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/TetWorldMetal.app/Contents/MacOS/TetWorldMetal}"
output_dir="${2:-${repo_root}/build/metal-sky-view-reference-qualification}"

if [[ ! -x "${binary}" ]]; then
  echo "release TetWorldMetal executable is missing: ${binary}" >&2
  exit 2
fi
mkdir -p "${output_dir}"

capture() {
  local resolution="$1" name="$2" selector="$3"
  local image="${output_dir}/${resolution}-${name}.ppm"
  local log="${output_dir}/${resolution}-${name}.jsonl"
  local -a environment=(TETWORLD_METAL_BACKGROUND=1)
  if [[ "${resolution}" == "384x216" ]]; then
    environment+=(TETWORLD_METAL_SKY_VIEW_REFERENCE=0)
  else
    environment+=(TETWORLD_METAL_SKY_VIEW_REFERENCE=1)
  fi
  case "${selector}" in
    mountain) environment+=(TETWORLD_METAL_REPORTED_MOUNTAIN=1) ;;
    sun) environment+=(TETWORLD_METAL_VISIBLE_SUN=1) ;;
    *) environment+=(TETWORLD_METAL_ATMOSPHERE_CAPTURE_POSE="${selector}") ;;
  esac
  env "${environment[@]}" "${binary}" --metal-atmosphere-capture "${image}" |
    tee "${log}"
  grep -q '"event":"metal_atmosphere_frame_smoke"' "${log}"
  grep -q '"passed":true' "${log}"
}

profile() {
  local resolution="$1" profile_class="$2"
  local log="${output_dir}/${resolution}-${profile_class}-profile.jsonl"
  local -a environment=(TETWORLD_METAL_BACKGROUND=1
                        TETWORLD_METAL_TIMING_PROFILE="${profile_class}")
  if [[ "${resolution}" == "384x216" ]]; then
    environment+=(TETWORLD_METAL_SKY_VIEW_REFERENCE=0)
  else
    environment+=(TETWORLD_METAL_SKY_VIEW_REFERENCE=1)
  fi
  # The lookup profile needs completed timestamp flights to distinguish new
  # work from retained counter values.  The moving profile intentionally uses
  # normal asynchronous submission because it qualifies user-visible motion.
  if [[ "${profile_class}" == "lookup-refresh" ]]; then
    environment+=(TETWORLD_METAL_STAGE_TIMESTAMPS=1
                  TETWORLD_METAL_SERIAL_STAGE_TIMESTAMPS=1)
  fi
  env "${environment[@]}" "${binary}" --metal-timing-profile-smoke-test |
    tee "${log}"
  grep -q '"event":"metal_timing_profile"' "${log}"
  grep -q '"passed":true' "${log}"
}

# Low-sun cases cover both terrain occultation and a directly visible disc.
# The named flight/top/orbit poses are native Metal fixtures whose coordinate
# system is tied to the production compact planet (ten metres per world unit).
for resolution in 384x216 200x100; do
  capture "${resolution}" mountain mountain
  capture "${resolution}" sun sun
  capture "${resolution}" flight flight
  capture "${resolution}" atmosphere-top atmosphere-top
  capture "${resolution}" orbit orbit
  capture "${resolution}" orbit-motion-a orbit-motion-a
  capture "${resolution}" orbit-motion-b orbit-motion-b
  profile "${resolution}" lookup-refresh
  profile "${resolution}" moving
done

python3 - "${output_dir}" <<'PY'
import json
import math
from pathlib import Path
import sys

root = Path(sys.argv[1])
names = (
    "mountain", "sun", "flight", "atmosphere-top", "orbit",
    "orbit-motion-a", "orbit-motion-b",
)

def image(path):
    data = path.read_bytes()
    fields = data.split(b"\n", 3)
    if len(fields) != 4 or fields[0] != b"P6":
        raise RuntimeError(f"invalid PPM: {path}")
    width, height = map(int, fields[1].split())
    if fields[2] != b"255" or len(fields[3]) != width * height * 3:
        raise RuntimeError(f"invalid PPM pixels: {path}")
    return width, height, fields[3]

def nrms(reference, candidate):
    if reference[:2] != candidate[:2]:
        raise RuntimeError("capture dimensions differ")
    squared = sum((a - b) ** 2 for a, b in zip(reference[2], candidate[2]))
    return math.sqrt(squared / len(reference[2])) / 255.0

def smoke(path):
    events = [json.loads(line) for line in path.read_text().splitlines()
              if line.startswith("{")]
    frames = [event for event in events
              if event.get("event") == "metal_atmosphere_frame_smoke"]
    if len(frames) != 1 or not frames[0].get("passed"):
        raise RuntimeError(f"failed or missing atmosphere smoke: {path}")
    return frames[0]

def profile(path):
    events = [json.loads(line) for line in path.read_text().splitlines()
              if line.startswith("{")]
    profiles = [event for event in events
                if event.get("event") == "metal_timing_profile"]
    if len(profiles) != 1 or not profiles[0].get("passed"):
        raise RuntimeError(f"failed or missing timing profile: {path}")
    return profiles[0]

matrix = {}
# Threshold is intentionally far above the already observed ground values,
# but tight enough to reject a visibly different LUT reconstruction.  The
# explicit per-pose result avoids hiding an orbital failure in a global RMS.
for name in names:
    baseline = image(root / f"384x216-{name}.ppm")
    candidate = image(root / f"200x100-{name}.ppm")
    value = nrms(baseline, candidate)
    if value > 0.004:
        raise SystemExit(f"{name} NRMS {value:.7f} exceeds 0.0040000")
    matrix[name] = {
        "nrms_vs_384x216": value,
        "baseline": smoke(root / f"384x216-{name}.jsonl"),
        "candidate": smoke(root / f"200x100-{name}.jsonl"),
    }

# Nearby orbital views must retain the same limb response at reduced LUT
# resolution.  This compares the change introduced by real motion, rather
# than falsely requiring two distinct physical views to be byte-identical.
baseline_drift = nrms(image(root / "384x216-orbit-motion-a.ppm"),
                      image(root / "384x216-orbit-motion-b.ppm"))
candidate_drift = nrms(image(root / "200x100-orbit-motion-a.ppm"),
                       image(root / "200x100-orbit-motion-b.ppm"))
if candidate_drift > baseline_drift + 0.002:
    raise SystemExit("200x100 orbital motion drift exceeds the 384x216 control")

baseline_refresh = profile(root / "384x216-lookup-refresh-profile.jsonl")
candidate_refresh = profile(root / "200x100-lookup-refresh-profile.jsonl")
baseline_moving = profile(root / "384x216-moving-profile.jsonl")
candidate_moving = profile(root / "200x100-moving-profile.jsonl")
if candidate_refresh["sky_view_lookup_ms"] >= baseline_refresh["sky_view_lookup_ms"]:
    raise SystemExit("200x100 did not improve the measured sky-view refresh")
if candidate_moving["p95_ms"] > baseline_moving["p95_ms"] + 1.0:
    raise SystemExit("200x100 moving p95 regresses by more than 1 ms")

result = {
    "event": "metal_sky_view_reference_qualification",
    "candidate": "200x100",
    "baseline": "384x216",
    "poses": matrix,
    "orbital_motion_nrms": {
        "baseline": baseline_drift,
        "candidate": candidate_drift,
    },
    "profiles": {
        "lookup_refresh": {
            "baseline_sky_view_ms": baseline_refresh["sky_view_lookup_ms"],
            "candidate_sky_view_ms": candidate_refresh["sky_view_lookup_ms"],
        },
        "moving": {
            "baseline_median_ms": baseline_moving["median_ms"],
            "baseline_p95_ms": baseline_moving["p95_ms"],
            "candidate_median_ms": candidate_moving["median_ms"],
            "candidate_p95_ms": candidate_moving["p95_ms"],
        },
    },
    "passed": True,
}
(root / "qualification.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result))
PY
