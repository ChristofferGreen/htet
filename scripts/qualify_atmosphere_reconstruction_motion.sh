#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/tetra_world}"
output_dir="${2:-${repo_root}/build/atmosphere-reconstruction-motion}"
window_size="${TETRA_ATMOSPHERE_MOTION_WINDOW_SIZE:-960x600}"
mkdir -p "${output_dir}"

common=(
  --window-size="${window_size}" --free-fly --surface-edges-off
  --atmosphere-preset=gameplay-planet --atmosphere-quality=default
  --atmosphere-transport=faithful-hillaire
  --atmosphere-shadow-integrator=minmax-segments
  --atmosphere-shadow-filter=physical-footprint --exposure-ev=-0.62
  --camera-yaw-degrees=137.6 --sun-azimuth-degrees=-49
  --sun-elevation-degrees=5
)

capture() {
  local method="$1" name="$2" feet="$3" pitch="$4"
  shift 4
  local method_arguments=()
  if [[ "${method}" == temporal-half-resolution ]]; then
    method_arguments+=(--atmosphere-screen-resolution-divisor=4)
  elif [[ "${method}" == deterministic-half-resolution ]]; then
    method_arguments+=(--atmosphere-screen-resolution-divisor=2)
  fi
  "${binary}" "${common[@]}" --camera-feet="${feet}" \
    --camera-pitch-degrees="${pitch}" \
    --atmosphere-rendering-method="${method}" \
    "${method_arguments[@]}" "$@" \
    --gpu-atmosphere-capture="${output_dir}/${name}-${method}.ppm" \
    >"${output_dir}/${name}-${method}.jsonl"
}

poses=(
  "pitch-a|0.500,0.500,0.780|-3.85"
  "pitch-b|0.500,0.500,0.780|-3.75"
  "translation-a|0.500,0.500,0.780|-3.80"
  "translation-b|0.510,0.490,0.780|-3.80"
)
for pose in "${poses[@]}"; do
  IFS='|' read -r name feet pitch <<<"${pose}"
  capture native-screen-oracle "${name}" "${feet}" "${pitch}"
  capture deterministic-half-resolution "${name}" "${feet}" "${pitch}"
  capture temporal-half-resolution "${name}" "${feet}" "${pitch}"
done

# These captures exercise reprojection in one process. The capture is taken
# after the scripted camera change has caused and completed a terrain update,
# rather than warming history only at the final static pose.
motion_cases=(
  "look-small|--automation-look=4,1"
  "look-large|--automation-look=18,-6"
  "walk|--automation-walk-steps=4"
)
for motion in "${motion_cases[@]}"; do
  IFS='|' read -r name argument <<<"${motion}"
  capture native-screen-oracle "${name}" 0.500,0.500,0.780 -3.80 \
    "${argument}" --gpu-atmosphere-capture-after-motion-frames=24
  capture temporal-half-resolution "${name}" 0.500,0.500,0.780 -3.80 \
    "${argument}" --gpu-atmosphere-capture-after-motion-frames=24
done

# Capture while a small look delta is still being applied every frame. This
# specifically rejects stale per-ray visibility that settled-frame captures
# cannot observe.
capture native-screen-oracle "look-continuous-frame-8" \
  0.500,0.500,0.780 -3.80 --automation-look=4,2 \
  --automation-look-frames=16 --gpu-atmosphere-capture-after-motion-frames=8
capture temporal-half-resolution "look-continuous-frame-8" \
  0.500,0.500,0.780 -3.80 --automation-look=4,2 \
  --automation-look-frames=16 --gpu-atmosphere-capture-after-motion-frames=8

report='[]'
names=()
for pose in "${poses[@]}"; do
  IFS='|' read -r name _ _ <<<"${pose}"
  names+=("${name}")
done
for motion in "${motion_cases[@]}"; do
  IFS='|' read -r name _ <<<"${motion}"
  names+=("${name}")
done
names+=("look-continuous-frame-8")
for name in "${names[@]}"; do
  oracle="${output_dir}/${name}-native-screen-oracle.ppm"
  mask="${output_dir}/${name}-native-screen-oracle.silhouette.pgm"
  for method in deterministic-half-resolution temporal-half-resolution; do
    candidate="${output_dir}/${name}-${method}.ppm"
    [[ -f "${candidate}" ]] || continue
    oracle_dimensions="$(magick identify -format '%wx%h' "${oracle}")"
    candidate_dimensions="$(magick identify -format '%wx%h' "${candidate}")"
    [[ "${oracle_dimensions}" == "${candidate_dimensions}" ]]
    global="$({ magick compare -metric RMSE "${oracle}" "${candidate}" \
        null: 2>&1 || true; } | sed -E 's/.*\(([^)]+)\).*/\1/')"
    silhouette="$({ magick compare -read-mask "${mask}" -metric RMSE \
        "${oracle}" "${candidate}" null: 2>&1 || true; } |
        sed -E 's/.*\(([^)]+)\).*/\1/')"
    awk -v global="${global}" -v silhouette="${silhouette}" \
        'BEGIN { exit !(global<=0.005 && silhouette<=0.01) }'
    report="$(jq -c --arg name "${name}" --arg method "${method}" \
        --argjson global "${global}" --argjson silhouette "${silhouette}" \
        '.+[{name:$name,method:$method,global_rmse:$global,
          silhouette_rmse:$silhouette}]' <<<"${report}")"
  done
done

magick montage "${output_dir}"/*.ppm -thumbnail 960x600 -tile 4x \
  -geometry +8+24 -background white -set label '%t' \
  "${output_dir}/motion-contact-sheet.png"
jq -n --arg window_size "${window_size}" --argjson poses "${report}" \
  '{event:"atmosphere_reconstruction_motion",status:"pass",
    window_size:$window_size,poses:$poses}' |
  tee "${output_dir}/qualification.json"
