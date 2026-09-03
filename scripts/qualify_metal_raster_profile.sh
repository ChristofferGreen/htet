#!/usr/bin/env bash
set -euo pipefail

# P6b: compare one fixed raster candidate with the established 0.7/2x MetalFX
# reference.  Both routes are native, hidden, and capture the final drawable.
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/TetWorldMetal.app/Contents/MacOS/TetWorldMetal}"
output_dir="${2:-${repo_root}/build/metal-raster-profile-qualification}"
candidate_scale="${3:-0.5}"
candidate_samples="${4:-4}"

if [[ ! -x "${binary}" ]]; then
  echo "release TetWorldMetal executable is missing: ${binary}" >&2
  exit 2
fi
mkdir -p "${output_dir}"

capture() {
  local prefix="$1" scale="$2" samples="$3" name="$4"
  local -a envvars=(TETWORLD_METAL_BACKGROUND=1
                     TETWORLD_METAL_RASTER_PROFILE_QUALIFICATION=1
                     TETWORLD_METAL_TIMING_PROFILE_SCALE="${scale}"
                     TETWORLD_METAL_TIMING_PROFILE_MSAA="${samples}"
                     TETWORLD_METAL_CAPTURE_INTERACTIVE_RESOLUTION=1)
  case "${name}" in
    mountain) envvars+=(TETWORLD_METAL_REPORTED_MOUNTAIN=1) ;;
    sun) envvars+=(TETWORLD_METAL_VISIBLE_SUN=1) ;;
    *) envvars+=(TETWORLD_METAL_ATMOSPHERE_CAPTURE_POSE="${name}") ;;
  esac
  env "${envvars[@]}" "${binary}" --metal-atmosphere-capture \
    "${output_dir}/${prefix}-${name}.ppm" | tee "${output_dir}/${prefix}-${name}.jsonl"
}

motion() {
  local prefix="$1" scale="$2" samples="$3" mode="$4"
  local label="${mode#--metal-}"
  label="${label%-test}"
  env TETWORLD_METAL_BACKGROUND=1 \
      TETWORLD_METAL_RASTER_PROFILE_QUALIFICATION=1 \
      TETWORLD_METAL_TIMING_PROFILE_SCALE="${scale}" \
      TETWORLD_METAL_TIMING_PROFILE_MSAA="${samples}" \
      "${binary}" "${mode}" | tee "${output_dir}/${prefix}-${label}.jsonl"
}

profile() {
  local prefix="$1" scale="$2" samples="$3" class="$4" repeat="$5"
  env TETWORLD_METAL_BACKGROUND=1 \
      TETWORLD_METAL_TIMING_PROFILE="${class}" \
      TETWORLD_METAL_TIMING_PROFILE_SCALE="${scale}" \
      TETWORLD_METAL_TIMING_PROFILE_MSAA="${samples}" \
      "${binary}" --metal-timing-profile-smoke-test | \
      tee "${output_dir}/${prefix}-${class}-${repeat}.jsonl"
}

names=(mountain sun flight atmosphere-top orbit orbit-motion-a orbit-motion-b)
for prefix_scale_samples in "control 0.7 2" "candidate ${candidate_scale} ${candidate_samples}"; do
  read -r prefix scale samples <<<"${prefix_scale_samples}"
  for name in "${names[@]}"; do capture "${prefix}" "${scale}" "${samples}" "${name}"; done
  motion "${prefix}" "${scale}" "${samples}" --metal-motion-smoke-test
  motion "${prefix}" "${scale}" "${samples}" --metal-metalfx-smoke-test
done
for repeat in 1 2; do
  for prefix_scale_samples in "control 0.7 2" "candidate ${candidate_scale} ${candidate_samples}"; do
    read -r prefix scale samples <<<"${prefix_scale_samples}"
    profile "${prefix}" "${scale}" "${samples}" stable "${repeat}"
    profile "${prefix}" "${scale}" "${samples}" moving "${repeat}"
  done
done

python3 - "${output_dir}" "${candidate_scale}" "${candidate_samples}" <<'PY'
import json, math, sys
from pathlib import Path

root, scale, samples = Path(sys.argv[1]), float(sys.argv[2]), int(sys.argv[3])
names = ("mountain", "sun", "flight", "atmosphere-top", "orbit",
         "orbit-motion-a", "orbit-motion-b")

def image(path):
    fields = path.read_bytes().split(b"\n", 3)
    if len(fields) != 4 or fields[0] != b"P6": raise SystemExit(f"invalid PPM: {path}")
    wh = tuple(map(int, fields[1].split()))
    if fields[2] != b"255" or len(fields[3]) != wh[0]*wh[1]*3: raise SystemExit(f"invalid pixels: {path}")
    return wh + (fields[3],)

def nrms(a, b):
    if a[:2] != b[:2]: raise SystemExit("capture dimensions differ")
    return math.sqrt(sum((x-y)**2 for x,y in zip(a[2],b[2]))/len(a[2]))/255.0

def event(path, kind):
    events = [json.loads(x) for x in path.read_text().splitlines() if x.startswith("{")]
    result = next((x for x in events if x.get("event") == kind), None)
    if not result or not result.get("passed"): raise SystemExit(f"failed/missing {kind}: {path}")
    return result

matrix = {}
for name in names:
    control = event(root/f"control-{name}.jsonl", "metal_atmosphere_frame_smoke")
    candidate = event(root/f"candidate-{name}.jsonl", "metal_atmosphere_frame_smoke")
    for row, expected_scale, expected_samples in ((control, .7, 2), (candidate, scale, samples)):
        if row.get("render_scale") != expected_scale or row.get("terrain_samples") != expected_samples or not row.get("metalfx"):
            raise SystemExit(f"raster identity mismatch: {name}")
    value = nrms(image(root/f"control-{name}.ppm"), image(root/f"candidate-{name}.ppm"))
    if value > .004: raise SystemExit(f"{name} NRMS {value:.7f} exceeds 0.004")
    matrix[name] = value

for prefix in ("control", "candidate"):
    for label, kind in (("motion-smoke", "metal_motion_smoke"),
                        ("metalfx-smoke", "metal_metalfx_smoke")):
        event(root/f"{prefix}-{label}.jsonl", kind)
control_drift = nrms(image(root/"control-orbit-motion-a.ppm"), image(root/"control-orbit-motion-b.ppm"))
candidate_drift = nrms(image(root/"candidate-orbit-motion-a.ppm"), image(root/"candidate-orbit-motion-b.ppm"))
if candidate_drift > control_drift + .002: raise SystemExit("candidate orbital motion drift exceeds control")
profiles = {}
for prefix in ("control", "candidate"):
    profiles[prefix] = {}
    for cls in ("stable", "moving"):
        values = [event(root/f"{prefix}-{cls}-{repeat}.jsonl", "metal_timing_profile") for repeat in (1,2)]
        profiles[prefix][cls] = {"median_ms": sum(x["median_ms"] for x in values)/2,
                                 "p95_ms": sum(x["p95_ms"] for x in values)/2}
result = {"event":"metal_raster_profile_qualification", "candidate":{"scale":scale,"samples":samples},
          "control":{"scale":.7,"samples":2}, "nrms":matrix,
          "orbital_motion":{"control":control_drift,"candidate":candidate_drift},
          "profiles":profiles, "passed":True}
(root/"qualification.json").write_text(json.dumps(result, indent=2)+"\n")
print(json.dumps(result))
PY
