#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/tetra_world}"
output_dir="${2:-${repo_root}/build/atmosphere-mountain-shadow}"
window_size="${TETRA_ATMOSPHERE_WINDOW_SIZE:-960x540}"
shadow_integrator="${TETRA_ATMOSPHERE_SHADOW_INTEGRATOR:-minmax-segments}"
shadow_filter="${TETRA_ATMOSPHERE_SHADOW_FILTER:-fixed-tent}"
surface_bias="${TETRA_SURFACE_SHADOW_BIAS:-slope-scaled}"

if [[ ! -x "${binary}" ]]; then
  echo "release tetra_world executable is missing: ${binary}" >&2
  exit 2
fi
mkdir -p "${output_dir}"

common_arguments=(
  --window-size="${window_size}"
  --free-fly
  --atmosphere-preset=gameplay-planet
  --atmosphere-quality=default
  --atmosphere-transport=faithful-hillaire
  --atmosphere-shadow-integrator="${shadow_integrator}"
  --atmosphere-shadow-filter="${shadow_filter}"
  --surface-shadow-bias="${surface_bias}"
  --exposure-ev=-0.62
  --camera-feet=0.5,0.5,0.78
  --camera-yaw-degrees=137.6
  --camera-pitch-degrees=-3.8
  --sun-azimuth-degrees=-49
  --sun-elevation-degrees=5
  --surface-edges-off
)

# This is the production pose that exposed an unshadowed Mie aureole through
# the central mountain while its asynchronous fitted shadow front was still
# incomplete.  Local cascades already contain the mountain in this state, so
# their long-shadow loss must remain consumable.  Capture metadata verifies
# the occultation instead of relying on apparent screen proximity.
capture() {
  local name="$1" debug="$2"
  shift 2
  local image="${output_dir}/${name}.ppm"
  local evidence="${output_dir}/${name}.jsonl"
  "${binary}" \
    "${common_arguments[@]}" \
    --atmosphere-debug="${debug}" \
    "$@" \
    --gpu-atmosphere-capture="${image}" >"${evidence}"
}

"${binary}" "${common_arguments[@]}" \
  --gpu-shadow-projection-probe >"${output_dir}/projection-probe.jsonl"
jq -e 'select(.event == "gpu_shadow_projection_probe") |
       .status == "pass" and .maximum_error < 0.00002 and
       ([.cases[].pass] | all)' \
  "${output_dir}/projection-probe.jsonl" >/dev/null

capture final 0
capture surface-direct 20
capture unobstructed-control 0 --sun-elevation-degrees=15
capture multiple-scattering 2
capture shadow-coverage 11
capture direct-shadow-loss 12
capture unshadowed-full-sky 13
capture shadowed-full-sky 14

final="$(grep '"event":"gpu_atmosphere_capture"' \
  "${output_dir}/final.jsonl")"
surface_direct="$(grep '"event":"gpu_atmosphere_capture"' \
  "${output_dir}/surface-direct.jsonl")"
unobstructed="$(grep '"event":"gpu_atmosphere_capture"' \
  "${output_dir}/unobstructed-control.jsonl")"
coverage="$(grep '"event":"gpu_atmosphere_capture"' \
  "${output_dir}/shadow-coverage.jsonl")"
loss="$(grep '"event":"gpu_atmosphere_capture"' \
  "${output_dir}/direct-shadow-loss.jsonl")"
unshadowed="$(grep '"event":"gpu_atmosphere_capture"' \
  "${output_dir}/unshadowed-full-sky.jsonl")"
shadowed="$(grep '"event":"gpu_atmosphere_capture"' \
  "${output_dir}/shadowed-full-sky.jsonl")"

jq -e '.sun_screen_visible and .sun_centre_geometry_occluded' \
  <<<"${final}" >/dev/null
jq -e '.sun_screen_visible and (.sun_centre_geometry_occluded | not) and
       .analysis.maximum[0] > 240 and .analysis.maximum[1] > 240' \
  <<<"${unobstructed}" >/dev/null
jq -e '.analysis.maximum[0] > 0 and
       .analysis.luminance_mean > 0.25' <<<"${coverage}" >/dev/null
jq -e '.analysis.maximum[0] > 64' <<<"${loss}" >/dev/null
python3 - "${output_dir}/final.ppm" "${output_dir}/final.jsonl" \
  "${output_dir}/surface-direct.ppm" <<'PY'
import json
import pathlib
import sys

image_path=pathlib.Path(sys.argv[1])
record=json.loads(pathlib.Path(sys.argv[2]).read_text().splitlines()[-1])
header=image_path.read_bytes().split(b'\n',3)
width,height=map(int,header[1].split())
pixels=header[3]
sun_x,sun_y=record['sun_pixel']
x0=max(0,sun_x-round(width*0.11))
x1=min(width,sun_x+round(width*0.11))
y0=max(0,sun_y-round(height*0.015))
y1=min(height,sun_y+round(height*0.18))
warmth=[]
for y in range(y0,y1):
    for x in range(x0,x1):
        offset=(y*width+x)*3
        warmth.append(pixels[offset]-pixels[offset+2])
mean_warmth=sum(warmth)/max(len(warmth),1)
if mean_warmth>=1.0:
    raise SystemExit(
        f'occluded-sun region retains a warm aureole: {mean_warmth:.3f}')

# The final colour can hide a narrow direct-scattering leak among shaded
# terrain.  Inspect the direct-only, surface-truncated diagnostic immediately
# below the occulted sun as a second guard against the triangular regression.
direct=pathlib.Path(sys.argv[3]).read_bytes().split(b'\n',3)
direct_width,direct_height=map(int,direct[1].split())
if (direct_width,direct_height)!=(width,height):
    raise SystemExit('surface-direct diagnostic dimensions do not match final')
direct_pixels=direct[3]
luminance=[]
for y in range(sun_y+round(height*0.03),sun_y+round(height*0.16)):
    for x in range(sun_x-round(width*0.05),sun_x+round(width*0.05)):
        offset=(y*width+x)*3
        red,green,blue=direct_pixels[offset:offset+3]
        luminance.append(0.2126*red+0.7152*green+0.0722*blue)
mean_direct=sum(luminance)/max(len(luminance),1)
if mean_direct>=10.0:
    raise SystemExit(
        f'occluded-sun direct Mie wedge remains: {mean_direct:.3f}')
PY
unshadowed_hash="$(jq -r '.rgb_hash' <<<"${unshadowed}")"
shadowed_hash="$(jq -r '.rgb_hash' <<<"${shadowed}")"
if [[ "${unshadowed_hash}" == "${shadowed_hash}" ]]; then
  echo "terrain shadow did not change full-sky radiance" >&2
  exit 3
fi

printf '%s\n' \
  "$(grep '"event":"gpu_shadow_projection_probe"' \
      "${output_dir}/projection-probe.jsonl")" \
  "${final}" "${surface_direct}" "${unobstructed}" "${coverage}" \
  "${loss}" "${unshadowed}" "${shadowed}"
