#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/tetra_world}"
output_dir="${2:-${repo_root}/build/atmosphere-mountain-shadow}"
window_size="${TETRA_ATMOSPHERE_WINDOW_SIZE:-960x540}"

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
  --exposure-ev=-0.62
  --camera-feet=0.5,0.72,0.78
  --camera-yaw-degrees=180
  --camera-pitch-degrees=0
  --sun-azimuth-degrees=-60
  --sun-elevation-degrees=3
)

# This pose puts the centre of the three-degree solar disc behind the left
# mountain in the deterministic production terrain.  The capture metadata
# verifies the occultation instead of relying on apparent screen proximity.
capture() {
  local name="$1" debug="$2"
  local image="${output_dir}/${name}.ppm"
  local evidence="${output_dir}/${name}.jsonl"
  "${binary}" \
    "${common_arguments[@]}" \
    --atmosphere-debug="${debug}" \
    --gpu-atmosphere-capture="${image}" >"${evidence}"
}

"${binary}" "${common_arguments[@]}" \
  --gpu-shadow-projection-probe >"${output_dir}/projection-probe.jsonl"
jq -e 'select(.event == "gpu_shadow_projection_probe") |
       .status == "pass" and .maximum_error < 0.00002 and
       ([.cases[].pass] | all)' \
  "${output_dir}/projection-probe.jsonl" >/dev/null

capture final 0
capture multiple-scattering 2
capture shadow-coverage 11
capture direct-shadow-loss 12
capture unshadowed-full-sky 13
capture shadowed-full-sky 14

final="$(grep '"event":"gpu_atmosphere_capture"' \
  "${output_dir}/final.jsonl")"
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
jq -e '.analysis.maximum[0] > 0 and
       .analysis.black_fraction > 0.05 and
       .analysis.black_fraction < 0.95' <<<"${coverage}" >/dev/null
jq -e '.analysis.maximum[0] > 0' <<<"${loss}" >/dev/null
unshadowed_hash="$(jq -r '.rgb_hash' <<<"${unshadowed}")"
shadowed_hash="$(jq -r '.rgb_hash' <<<"${shadowed}")"
if [[ "${unshadowed_hash}" == "${shadowed_hash}" ]]; then
  echo "terrain shadow did not change full-sky radiance" >&2
  exit 3
fi

printf '%s\n' \
  "$(grep '"event":"gpu_shadow_projection_probe"' \
      "${output_dir}/projection-probe.jsonl")" \
  "${final}" "${coverage}" "${loss}" "${unshadowed}" "${shadowed}"
