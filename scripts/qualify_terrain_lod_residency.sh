#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/tetra_world}"
output_dir="${2:-${repo_root}/artifacts/lod-residency-qualification}"
display_name="${3:-P34WD-40}"
mkdir -p "${output_dir}"

common=(
  --display-name="${display_name}" --window-size=1280x800
  --free-fly --surface-edges-off
  --atmosphere-preset=gameplay-planet --atmosphere-quality=default
  --atmosphere-transport=reference-hillaire-2020
  --atmosphere-screen-resolution-divisor=2 --exposure-ev=-0.62
  --camera-feet=0.5,0.5,0.78 --camera-yaw-degrees=131.7
  --camera-pitch-degrees=-5.7 --sun-azimuth-degrees=-49
  --sun-elevation-degrees=5
)

capture() {
  local name="$1"
  shift
  "${binary}" "${common[@]}" "$@" \
    --gpu-atmosphere-capture="${output_dir}/${name}.ppm" \
    >"${output_dir}/${name}.jsonl"
}

capture exact-early-frame-2 --gpu-atmosphere-capture-frame=2
capture exact-settled
capture rotation-a-b-a \
  --automation-yaw-sequence-degrees=180,-180 \
  --automation-look-frames=16 \
  --gpu-atmosphere-capture-after-motion-frames=120
capture rotation-four-quarter-turns \
  --automation-yaw-sequence-degrees=90,90,90,90 \
  --automation-look-frames=16 \
  --gpu-atmosphere-capture-after-motion-frames=120

python3 - "${output_dir}" "${display_name}" <<'PY'
import json
import pathlib
import sys

output = pathlib.Path(sys.argv[1])
display = sys.argv[2]

def capture_event(name):
    events = []
    for line in (output / f"{name}.jsonl").read_text().splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == "gpu_atmosphere_capture":
            events.append(event)
    if len(events) != 1:
        raise SystemExit(f"{name}: expected one capture event, got {len(events)}")
    event = events[0]
    if event.get("display_name") != display:
        raise SystemExit(
            f"{name}: ran on {event.get('display_name')!r}, expected {display!r}")
    if (event.get("logical_width"), event.get("logical_height")) != (1280, 800):
        raise SystemExit(f"{name}: unexpected logical window dimensions")
    return event

early = capture_event("exact-early-frame-2")
if early.get("runtime_rendered_frame") != 2:
    raise SystemExit("early capture was not terrain runtime frame 2")

settled = capture_event("exact-settled")
if settled.get("budget_exceeded") or settled.get("resident_sector_count", 0) < 1:
    raise SystemExit("settled exact pose did not publish a resident terrain front")

aba = capture_event("rotation-a-b-a")
if aba.get("gpu_ready_sector_count", 0) < 2:
    raise SystemExit("A-B-A did not retain both directional sectors GPU-ready")
if aba.get("submitted_builds") != 2:
    raise SystemExit(
        f"A-B-A scheduled {aba.get('submitted_builds')} builds; expected startup plus B only")

quarters = capture_event("rotation-four-quarter-turns")
if quarters.get("gpu_ready_sector_count", 0) < 4:
    raise SystemExit("four quarter turns did not retain four GPU-ready sectors")
if quarters.get("budget_exceeded"):
    raise SystemExit("four quarter turns exceeded a declared resource budget")

print(json.dumps({
    "event": "terrain_lod_residency_qualification",
    "display_name": display,
    "early_runtime_frame": early["runtime_rendered_frame"],
    "aba_gpu_ready_sectors": aba["gpu_ready_sector_count"],
    "aba_submitted_builds": aba["submitted_builds"],
    "quarter_turn_gpu_ready_sectors": quarters["gpu_ready_sector_count"],
}, separators=(",", ":")))
PY
