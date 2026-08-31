#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/tetra_world}"
output_dir="${2:-${repo_root}/build/gate-l-benchmarks}"
mkdir -p "${output_dir}"

common=(
  --gpu-atmosphere-benchmark --window-size=640x360 --free-fly
  --analytic-ridge --surface-edges-off
  --atmosphere-preset=gameplay-planet
  --atmosphere-transport=faithful-hillaire
  --atmosphere-shadow-filter=physical-footprint --exposure-ev=-0.62
  --camera-feet=0.5,4.0,2.0 --camera-yaw-degrees=180
  --camera-pitch-degrees=0 --sun-azimuth-degrees=-61.3
  --sun-elevation-degrees=2
)

for quality in low default high; do
  for integrator in minmax-segments epipolar-minmax moment-hybrid; do
    output="${output_dir}/${quality}-${integrator}.jsonl"
    "${binary}" "${common[@]}" --atmosphere-quality="${quality}" \
      --atmosphere-shadow-integrator="${integrator}" >"${output}"
  done
done

python3 - "${output_dir}" <<'PY'
import json
from pathlib import Path
import sys

root=Path(sys.argv[1])
records=[]
for path in sorted(root.glob('*.jsonl')):
    record=json.loads(path.read_text().splitlines()[-1])
    shadow=record['atmosphere_shadow']
    records.append({
        'quality':path.name.split('-',1)[0],
        'integrator':shadow['integrator'],
        'shadow_ms':record['shadows']['median_ms'],
        'shadow_p95_ms':record['shadows']['p95_ms'],
        'shadow_initial_refresh_maximum_ms':record['shadows']['initial_refresh_maximum_ms'],
        'atmosphere_ms':record['atmosphere']['median_ms'],
        'atmosphere_initial_refresh_maximum_ms':record['atmosphere']['initial_refresh_maximum_ms'],
        'composite_ms':record['composite']['median_ms'],
        'atmosphere_bytes':record['atmosphere_bytes'],
        'epipolar_visited_nodes':shadow.get('epipolar_visited_nodes',0),
        'epipolar_emitted_intervals':shadow.get('epipolar_emitted_intervals',0),
        'epipolar_fallbacks':shadow.get('epipolar_fallbacks',0),
        'epipolar_overflows':shadow.get('epipolar_overflows',0),
        'epipolar_hierarchy_refreshes':shadow.get('epipolar_hierarchy_refreshes',0),
    })
(root/'summary.json').write_text(json.dumps(records,indent=2)+'\n')
print(json.dumps({'event':'gate_l_benchmark_summary','records':records}))
PY
