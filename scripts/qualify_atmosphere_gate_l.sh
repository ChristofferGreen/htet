#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/tetra_world}"
output_dir="${2:-${repo_root}/build/gate-l-qualification}"
mkdir -p "${output_dir}"

common=(
  --window-size=640x360 --free-fly --analytic-ridge --surface-edges-off
  --atmosphere-preset=gameplay-planet --atmosphere-quality=low
  --atmosphere-transport=faithful-hillaire --exposure-ev=-0.62
  --shadow-raster-constant=0.8125 --shadow-raster-slope=1.21875
  --camera-feet=0.5,4.0,2.0 --camera-yaw-degrees=180
  --sun-azimuth-degrees=-61.3 --sun-elevation-degrees=2
  --atmosphere-debug=12
)

capture() {
  local name="$1" integrator="$2" filter="$3" pitch="$4"
  local directory="${output_dir}/${name}"
  mkdir -p "${directory}"
  if [[ -s "${directory}/direct-loss.ppm" &&
        -s "${directory}/direct-loss.jsonl" ]]; then
    return
  fi
  "${binary}" "${common[@]}" \
    --atmosphere-shadow-integrator="${integrator}" \
    --atmosphere-shadow-filter="${filter}" \
    --camera-pitch-degrees="${pitch}" \
    --gpu-atmosphere-capture="${directory}/direct-loss.ppm" \
    >"${directory}/direct-loss.jsonl"
}

capture oracle dense-oracle physical-footprint 0
capture minmax-physical minmax-segments physical-footprint 0
capture epipolar-unfiltered epipolar-minmax unfiltered 0
capture epipolar-fixed epipolar-minmax fixed-tent 0
capture epipolar-physical epipolar-minmax physical-footprint 0
capture moment-physical moment-hybrid physical-footprint 0
capture epipolar-motion-minus epipolar-minmax physical-footprint -0.02
capture epipolar-motion-plus epipolar-minmax physical-footprint 0.02

capture_surface() {
  local name="$1" surface_bias="$2" raster_constant="$3" raster_slope="$4"
  local directory="${output_dir}/${name}"
  mkdir -p "${directory}"
  if [[ -s "${directory}/surface.ppm" &&
        -s "${directory}/surface.jsonl" ]]; then
    return
  fi
  "${binary}" --window-size=640x360 --free-fly --surface-edges-off \
    --atmosphere-preset=gameplay-planet --atmosphere-quality=low \
    --atmosphere-transport=faithful-hillaire --exposure-ev=-0.62 \
    --camera-feet=0.5,0.5,0.78 --camera-yaw-degrees=137.6 \
    --camera-pitch-degrees=-3.8 --sun-azimuth-degrees=-49 \
    --sun-elevation-degrees=5 \
    --atmosphere-shadow-integrator=minmax-segments \
    --atmosphere-shadow-filter=physical-footprint \
    --surface-shadow-bias="${surface_bias}" \
    --shadow-raster-constant="${raster_constant}" \
    --shadow-raster-slope="${raster_slope}" \
    --atmosphere-debug=0 \
    --gpu-atmosphere-capture="${directory}/surface.ppm" \
    >"${directory}/surface.jsonl"
}

capture_surface surface-slope slope-scaled 0.8125 1.21875
capture_surface surface-receiver receiver-plane 0.8125 1.21875
capture_surface raster-zero slope-scaled 0 0
capture_surface raster-low slope-scaled 0.5 0.75
capture_surface raster-high slope-scaled 2.5 3.5

capture_comparison_bias() {
  local name="$1" bias="$2"
  local directory="${output_dir}/${name}"
  mkdir -p "${directory}"
  if [[ -s "${directory}/final.ppm" && -s "${directory}/final.jsonl" ]]; then
    return
  fi
  local override=()
  if [[ "${bias}" != auto ]]; then
    override+=(--atmosphere-comparison-bias-world="${bias}")
  fi
  "${binary}" --window-size=640x360 --free-fly --surface-edges-off \
    --atmosphere-preset=gameplay-planet --atmosphere-quality=low \
    --atmosphere-transport=faithful-hillaire --exposure-ev=-0.62 \
    --camera-feet=0.5,0.5,0.78 --camera-yaw-degrees=137.6 \
    --camera-pitch-degrees=-3.8 --sun-azimuth-degrees=-49 \
    --sun-elevation-degrees=5 --atmosphere-shadow-integrator=minmax-segments \
    --atmosphere-shadow-filter=physical-footprint --atmosphere-debug=0 \
    "${override[@]}" --gpu-atmosphere-capture="${directory}/final.ppm" \
    >"${directory}/final.jsonl"
}

capture_comparison_bias comparison-zero 0
capture_comparison_bias comparison-low 0.0018
capture_comparison_bias comparison-local 0.0036
capture_comparison_bias comparison-auto auto
capture_comparison_bias comparison-high 0.02

python3 "${repo_root}/scripts/compare_atmosphere_shadow_integrators.py" \
  "${output_dir}/oracle" "${output_dir}/minmax-physical" \
  "${output_dir}/epipolar-unfiltered" "${output_dir}/epipolar-fixed" \
  "${output_dir}/epipolar-physical" "${output_dir}/moment-physical" \
  >"${output_dir}/comparisons.jsonl"

python3 - "${output_dir}" <<'PY'
import json
from pathlib import Path
import sys
import numpy as np
from PIL import Image

root=Path(sys.argv[1])
comparisons={r['candidate']:r for r in map(json.loads,
    (root/'comparisons.jsonl').read_text().splitlines())}
physical=comparisons['epipolar-physical']
fixed=comparisons['epipolar-fixed']
unfiltered=comparisons['epipolar-unfiltered']
moment=comparisons['moment-physical']
if physical['normalized_rgb_rmse']>0.001 or physical['maximum_rgb_error']>0.02:
    raise SystemExit(f'epipolar physical-footprint path diverged: {physical}')
if physical['shadow_boundary_distance_pixels']>1.0:
    raise SystemExit(f'epipolar boundary moved: {physical}')
if (physical['outside_reference_mean_excess']>1.0e-5 or
        physical['outside_reference_maximum_excess']>1.0/255.0):
    raise SystemExit(f'epipolar filtering created energy outside cone: {physical}')
if physical['normalized_rgb_rmse']>min(fixed['normalized_rgb_rmse'],
                                       unfiltered['normalized_rgb_rmse']):
    raise SystemExit('physical visibility-footprint filtering did not improve the reference')
if moment['maximum_rgb_error']>0.02:
    raise SystemExit(f'moment fallback leaked light: {moment}')

minus=np.asarray(Image.open(root/'epipolar-motion-minus/direct-loss.ppm'),
                 dtype=np.float64)/255.0
plus=np.asarray(Image.open(root/'epipolar-motion-plus/direct-loss.ppm'),
                dtype=np.float64)/255.0
difference=np.abs(minus-plus)
changed_over_two=np.count_nonzero(np.max(difference,axis=2)>2.0/255.0)
if (np.sqrt(np.mean(difference*difference))>0.001 or
        changed_over_two>1000):
    raise SystemExit('epipolar sub-texel motion popped')

diagnostic=json.loads(
    (root/'epipolar-physical/direct-loss.jsonl').read_text().splitlines()[-1])
shadow=diagnostic['shadow_diagnostics']
if not shadow['hierarchy_complete'] or shadow['epipolar_overflows']!=0:
    raise SystemExit(f'epipolar hierarchy incomplete or overflowed: {shadow}')

surface_slope=np.asarray(Image.open(root/'surface-slope/surface.ppm'),
                         dtype=np.int16)
surface_receiver=np.asarray(Image.open(root/'surface-receiver/surface.ppm'),
                            dtype=np.int16)
surface_difference=np.abs(surface_slope-surface_receiver)
if surface_receiver.max()==0:
    raise SystemExit('receiver-plane surface bias produced an empty image')
if np.mean(surface_difference)>8.0:
    raise SystemExit('receiver-plane surface bias diverged grossly from slope bias')

for name in ('raster-zero','raster-low','surface-slope','raster-high'):
    record=json.loads((root/name/'surface.jsonl').read_text().splitlines()[-1])
    if record['analysis']['clipped_fraction']>0.001:
        raise SystemExit(f'raster-bias sweep clipped output: {name}')
expected_raster={'raster-zero':(0.0,0.0),'raster-low':(0.5,0.75),
                 'surface-slope':(0.8125,1.21875),
                 'raster-high':(2.5,3.5)}
for name,(constant,slope) in expected_raster.items():
    record=json.loads((root/name/'surface.jsonl').read_text().splitlines()[-1])
    diagnostic=record['shadow_diagnostics']
    if (diagnostic['raster_bias_constant']!=constant or
            diagnostic['raster_bias_slope']!=slope):
        raise SystemExit(f'raster-bias CLI was not applied: {name}')

comparison_records=[]
for name in ('comparison-zero','comparison-low','comparison-local',
             'comparison-auto','comparison-high'):
    record=json.loads((root/name/'final.jsonl').read_text().splitlines()[-1])
    if record['analysis']['clipped_fraction']>0.001:
        raise SystemExit(f'comparison-bias sweep clipped output: {name}')
    comparison_records.append((name,record['shadow_diagnostics'][
        'comparison_bias_world']))
print(json.dumps({'event':'gate_l_qualification','physical':physical,
                  'moment':moment,'motion_rmse':float(np.sqrt(np.mean(
                      difference*difference))),
                  'motion_maximum':float(difference.max()),
                  'motion_pixels_over_two':int(changed_over_two),
                  'receiver_plane_mean_difference':float(
                      np.mean(surface_difference)),
                  'comparison_biases':comparison_records,
                  'shadow_diagnostics':shadow}))
PY
