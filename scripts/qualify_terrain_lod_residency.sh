#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/tetra_world}"
output_dir="${2:-${repo_root}/artifacts/lod-residency-qualification}"
display_name="${3:-P34WD-40}"
mkdir -p "${output_dir}"

if [[ "$(uname -s)" == "Darwin" ]] && command -v caffeinate >/dev/null 2>&1; then
  # Keep the selected display awake across all four separate capture
  # processes. A short synthetic activity pulse lets the first GLFW process
  # see a monitor but permits it to disappear again before later captures.
  caffeinate -dimsu -w "$$" &
  caffeinate -u -t 2
fi

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
capture exact-settled-wireframe --surface-edges-on
capture rotation-a-b \
  --automation-yaw-sequence-degrees=180 \
  --automation-look-frames=1 \
  --gpu-atmosphere-capture-after-motion-frames=120
capture rotation-a-b-a \
  --automation-yaw-sequence-degrees=180,-180 \
  --automation-look-frames=1 \
  --gpu-atmosphere-capture-after-motion-frames=120
capture rotation-four-quarter-turns \
  --automation-yaw-sequence-degrees=90,90,90,90 \
  --automation-look-frames=1 \
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
capture_event("exact-settled-wireframe")
settled_lod = settled["terrain_lod"]
if settled_lod.get("budget_exceeded") or settled_lod.get("resident_sectors", 0) < 1:
    raise SystemExit("settled exact pose did not publish a resident terrain front")

ab = capture_event("rotation-a-b")
ab_lod = ab["terrain_lod"]
if ab_lod.get("gpu_ready_sectors", 0) < 2:
    raise SystemExit("A-B did not retain both directional sectors GPU-ready")

aba = capture_event("rotation-a-b-a")
aba_lod = aba["terrain_lod"]
if aba_lod.get("gpu_ready_sectors", 0) < 2:
    raise SystemExit("A-B-A did not retain both directional sectors GPU-ready")
if aba_lod.get("submitted_builds") != 2:
    raise SystemExit(
        f"A-B-A scheduled {aba_lod.get('submitted_builds')} builds; expected startup plus B only")
aba_pacing = aba["frame_pacing_summary"]
if aba_pacing["missed_present_count"] != 0:
    raise SystemExit("cached return to A missed the 120 Hz presentation budget")
if aba_pacing["gpu"]["p95_ms"] > aba_pacing["present_budget_ms"]:
    raise SystemExit("cached return to A exceeded the 120 Hz GPU p95 budget")
if aba_pacing["cpu"]["p95_ms"] > aba_pacing["present_budget_ms"] * 1.5:
    raise SystemExit("cached return to A contains a distinct long-frame population")
if aba_pacing["terrain_construction"]["maximum_ms"] != 0:
    raise SystemExit("cached return to A performed terrain construction")
if aba_pacing["upload"]["maximum_ms"] != 0:
    raise SystemExit("cached return to A performed a terrain upload")
for metric in ("visible_edge_p95", "visible_field_error_p95",
               "visible_limb_error_p95"):
    if aba_lod[metric] > settled_lod[metric] * (1.0 + 1.0e-9):
        raise SystemExit(f"cached return to A increased {metric}")

quarters = capture_event("rotation-four-quarter-turns")
quarters_lod = quarters["terrain_lod"]
if quarters_lod.get("resident_sectors", 0) < 4:
    raise SystemExit("four quarter turns did not retain four logical sectors")
if quarters_lod.get("budget_exceeded"):
    raise SystemExit("four quarter turns exceeded a declared resource budget")
if (quarters_lod.get("gpu_ready_sectors", 0) < 4 and
        quarters_lod.get("sector_demotions", 0) == 0):
    raise SystemExit("quarter-turn GPU demotion was not reported explicitly")

print(json.dumps({
    "event": "terrain_lod_residency_qualification",
    "display_name": display,
    "early_runtime_frame": early["runtime_rendered_frame"],
    "aba_gpu_ready_sectors": aba_lod["gpu_ready_sectors"],
    "aba_submitted_builds": aba_lod["submitted_builds"],
    "aba_gpu_p95_ms": aba_pacing["gpu"]["p95_ms"],
    "quarter_turn_resident_sectors": quarters_lod["resident_sectors"],
    "quarter_turn_gpu_ready_sectors": quarters_lod["gpu_ready_sectors"],
    "quarter_turn_demotions": quarters_lod["sector_demotions"],
}, separators=(",", ":")))
PY
