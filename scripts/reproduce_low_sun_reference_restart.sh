#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/tetra_world}"
output_dir="${2:-${repo_root}/build/low-sun-reference-restart}"
display_name="${3:-P34WD-40}"
mkdir -p "${output_dir}"

if [[ "$(uname -s)" == "Darwin" ]] && command -v caffeinate >/dev/null 2>&1; then
  caffeinate -u -t 2
fi

common=(
  --display-name="${display_name}" --window-size=1280x800
  --free-fly --surface-edges-off
  --atmosphere-preset=gameplay-planet --atmosphere-quality=default
  --atmosphere-screen-resolution-divisor=2 --exposure-ev=-0.62
  --camera-feet=0.5,0.5,0.78 --camera-yaw-degrees=131.7
  --camera-pitch-degrees=-5.7 --sun-azimuth-degrees=-49
  --sun-elevation-degrees=5
)

capture() {
  local transport="$1" frame="$2" debug="$3"
  local name="${transport}-frame-${frame}-debug-${debug}"
  "${binary}" "${common[@]}" \
    --atmosphere-transport="${transport}" \
    --atmosphere-debug="${debug}" \
    --gpu-atmosphere-capture-frame="${frame}" \
    --gpu-atmosphere-capture="${output_dir}/${name}.ppm" \
    >"${output_dir}/${name}.jsonl"
}

capture_settled() {
  local transport="$1"
  local name="${transport}-settled-debug-0"
  "${binary}" "${common[@]}" \
    --atmosphere-transport="${transport}" --atmosphere-debug=0 \
    --gpu-atmosphere-capture="${output_dir}/${name}.ppm" \
    >"${output_dir}/${name}.jsonl"
}

capture_motion() {
  local name="reference-hillaire-2020-motion-frame-8"
  "${binary}" "${common[@]}" \
    --atmosphere-transport=reference-hillaire-2020 \
    --atmosphere-debug=0 --automation-look=4,2 \
    --automation-look-frames=16 \
    --gpu-atmosphere-capture-after-motion-frames=8 \
    --gpu-atmosphere-capture="${output_dir}/${name}.ppm" \
    >"${output_dir}/${name}.jsonl"
}

for transport in faithful-hillaire reference-hillaire-2020; do
  for frame in 1 2 4 8 16 64; do
    capture "${transport}" "${frame}" 0
  done
  for debug in 25 26 27; do
    capture "${transport}" 16 "${debug}"
  done
  capture_settled "${transport}"
done

capture_motion
