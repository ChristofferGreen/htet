#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/tetra_world}"
output_dir="${2:-${repo_root}/build/atmosphere-analytic-ridge}"
shadow_integrator="${TETRA_ATMOSPHERE_SHADOW_INTEGRATOR:-adaptive-transition}"
shadow_filter="${TETRA_ATMOSPHERE_SHADOW_FILTER:-fixed-tent}"
mkdir -p "${output_dir}"

common=(
  --window-size=960x540 --free-fly --analytic-ridge --surface-edges-off
  --atmosphere-preset=gameplay-planet --atmosphere-quality=default
  --atmosphere-transport=faithful-hillaire --exposure-ev=-0.62
  --atmosphere-shadow-integrator="${shadow_integrator}"
  --atmosphere-shadow-filter="${shadow_filter}"
  --camera-feet=0.5,4.0,2.0 --camera-yaw-degrees=180
  --camera-pitch-degrees=0 --sun-azimuth-degrees=-61.3
  --sun-elevation-degrees=2
)

capture() {
  local name="$1" debug="$2"
  "${binary}" "${common[@]}" --atmosphere-debug="${debug}" \
    --gpu-atmosphere-capture="${output_dir}/${name}.ppm" \
    >"${output_dir}/${name}.jsonl"
}

capture final 0
capture direct-loss 12
capture unshadowed 13
capture shadowed 14

jq -e 'select(.event=="gpu_atmosphere_capture") |
  .sun_centre_geometry_occluded and .analysis.black_fraction < 0.001 and
  .analysis.clipped_fraction < 0.001 and .analysis.geometry_fraction > 0.3 and
  .analysis.geometry_fraction < 0.8' "${output_dir}/final.jsonl" >/dev/null
jq -e 'select(.event=="gpu_atmosphere_capture") |
  .analysis.maximum[0] > 4' "${output_dir}/direct-loss.jsonl" >/dev/null

test "$(jq -r '.rgb_hash' "${output_dir}/unshadowed.jsonl")" != \
     "$(jq -r '.rgb_hash' "${output_dir}/shadowed.jsonl")"

# Treat the exact triangular field as an analytic ridge and its extracted
# silhouette as the finite-resolution receiver boundary. Every meaningful
# clear-air direct-loss component must remain attached to that boundary; a
# detached component is the characteristic light-leak/floating-shaft failure.
python3 - "${output_dir}" <<'PY'
from collections import deque
from pathlib import Path
import sys

root=Path(sys.argv[1])
def image(path):
    header=path.read_bytes().split(b'\n',3)
    width,height=map(int,header[1].split())
    return width,height,header[3]

width,height,rgb=image(root/'direct-loss.ppm')
_,_,clear=image(root/'direct-loss.clear.pgm')
_,_,silhouette=image(root/'direct-loss.silhouette.pgm')
active=bytearray(width*height)
for index in range(width*height):
    active[index]=clear[index]>0 and max(rgb[3*index:3*index+3])>=4
active_count=sum(active)
if active_count<1000:
    raise SystemExit('analytic ridge produced no resolved clear-air shadow cone')

components=[]
for seed in range(width*height):
    if not active[seed]:
        continue
    queue=[seed];active[seed]=0;size=0;touches=False
    for point in queue:
        size+=1;x=point%width;y=point//width
        for yy in range(max(0,y-2),min(height,y+3)):
            for xx in range(max(0,x-2),min(width,x+3)):
                touches |= silhouette[yy*width+xx]>0
        for neighbour in (point-1,point+1,point-width,point+width):
            if (0<=neighbour<width*height and active[neighbour] and
                    abs(neighbour%width-x)<=1):
                active[neighbour]=0;queue.append(neighbour)
    if size>=32:
        components.append((size,touches))
if not components or len(components)>8 or any(not touch for _,touch in components):
    raise SystemExit(f'detached or fragmented analytic ridge shadow: {components}')
PY

grep '"event":"gpu_atmosphere_capture"' "${output_dir}"/*.jsonl
