#!/usr/bin/env bash
set -euo pipefail

# Native Metal P6a timing matrix.  The app owns the physical renderer; this
# script only fixes one scale/sample row at a time so no adaptive resolution
# change can make rows incomparable.
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/TetWorldMetal.app/Contents/MacOS/TetWorldMetal}"
output_dir="${2:-${repo_root}/build/metal-msaa-matrix}"

if [[ ! -x "${binary}" ]]; then
  echo "release TetWorldMetal executable is missing: ${binary}" >&2
  exit 2
fi
mkdir -p "${output_dir}"

for scale in 0.5 0.7 1.0; do
  for samples in 1 2 4; do
    for profile in stable moving; do
      name="scale-${scale}-msaa-${samples}-${profile}"
      env TETWORLD_METAL_BACKGROUND=1 \
          TETWORLD_METAL_TIMING_PROFILE="${profile}" \
          TETWORLD_METAL_TIMING_PROFILE_SCALE="${scale}" \
          TETWORLD_METAL_TIMING_PROFILE_MSAA="${samples}" \
          "${binary}" --metal-timing-profile-smoke-test |
        tee "${output_dir}/${name}.jsonl"
    done
  done
done

python3 - "${output_dir}" <<'PY'
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
rows = []
for path in sorted(root.glob("*.jsonl")):
    events = [json.loads(line) for line in path.read_text().splitlines()
              if line.startswith("{")]
    event = next((x for x in events
                  if x.get("event") == "metal_timing_profile"), None)
    if event is None or not event.get("passed"):
        raise SystemExit(f"missing or failed profile: {path}")
    parts = path.stem.split("-")
    scale = float(parts[1])
    samples = int(parts[3])
    width, height = map(int, event["drawable"].split("x"))
    internal = tuple(map(int, event["internal"].split("x")))
    expected = (round(width * scale), round(height * scale))
    if internal != expected:
        raise SystemExit(f"internal extent mismatch: {path}: {internal} != {expected}")
    if event["samples_per_pixel"] != samples:
        raise SystemExit(f"MSAA mismatch: {path}")
    if event["metalfx"] != (scale < 1.0):
        raise SystemExit(f"MetalFX state mismatch: {path}")
    rows.append({"row": path.stem, "median_ms": event["median_ms"],
                 "p95_ms": event["p95_ms"], "internal": event["internal"],
                 "metalfx": event["metalfx"]})
print(json.dumps({"event": "metal_msaa_matrix", "rows": rows}, indent=2))
PY
