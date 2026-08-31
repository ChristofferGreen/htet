#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/tetra_world}"
output_dir="${2:-${repo_root}/build/atmosphere-reconstruction}"
window_size="${TETRA_ATMOSPHERE_RECONSTRUCTION_WINDOW_SIZE:-1280x800}"

if [[ ! -x "${binary}" ]]; then
  echo "release tetra_world executable is missing: ${binary}" >&2
  exit 2
fi
if ! command -v magick >/dev/null 2>&1; then
  echo "ImageMagick is required for reconstruction qualification" >&2
  exit 2
fi
mkdir -p "${output_dir}"

common=(
  --window-size="${window_size}" --free-fly --surface-edges-off
  --atmosphere-preset=gameplay-planet --atmosphere-quality=default
  --atmosphere-transport=faithful-hillaire
  --atmosphere-shadow-integrator=minmax-segments
  --atmosphere-shadow-filter=physical-footprint --exposure-ev=-0.62
  --camera-feet=0.5,0.5,0.78 --camera-yaw-degrees=137.6
  --camera-pitch-degrees=-3.8 --sun-azimuth-degrees=-49
  --sun-elevation-degrees=5
)

capture() {
  local method="$1"
  local name="$2"
  shift 2
  "${binary}" "${common[@]}" --atmosphere-rendering-method="${method}" \
    "$@" \
    --gpu-atmosphere-capture="${output_dir}/${name}.ppm" \
    >"${output_dir}/${name}.jsonl"
  jq -e --arg method "${method}" \
    'select(.event=="gpu_atmosphere_capture") |
     .rendering_method==$method and .analysis.black_fraction<1 and
     .analysis.clipped_fraction<1' \
    "${output_dir}/${name}.jsonl" >/dev/null
}

capture native-screen-oracle oracle
capture deterministic-half-resolution deterministic-half \
  --atmosphere-screen-resolution-divisor=2
for divisor in 2 3 4; do
  capture temporal-half-resolution "temporal-divisor-${divisor}" \
    --atmosphere-screen-resolution-divisor="${divisor}"
done

oracle_dimensions="$(magick identify -format '%wx%h' "${output_dir}/oracle.ppm")"
for candidate in deterministic-half temporal-divisor-2 temporal-divisor-3 \
                 temporal-divisor-4; do
  candidate_dimensions="$(magick identify -format '%wx%h' \
    "${output_dir}/${candidate}.ppm")"
  if [[ "${candidate_dimensions}" != "${oracle_dimensions}" ]]; then
    echo "capture dimension mismatch: oracle=${oracle_dimensions}, ${candidate}=${candidate_dimensions}" >&2
    exit 1
  fi
done

metric() {
  local candidate="$1"
  local mask="$2"
  local arguments=()
  if [[ -n "${mask}" ]]; then arguments=(-read-mask "${mask}"); fi
  { magick compare "${arguments[@]}" -metric RMSE \
    "${output_dir}/oracle.ppm" "${output_dir}/${candidate}.ppm" null: 2>&1 || true; } |
    sed -E 's/.*\(([^)]+)\).*/\1/'
}

report='[]'
for candidate in deterministic-half temporal-divisor-2 temporal-divisor-3 \
                 temporal-divisor-4; do
  global_rmse="$(metric "${candidate}" '')"
  silhouette_rmse="$(metric "${candidate}" "${output_dir}/oracle.silhouette.pgm")"
  clear_rmse="$(metric "${candidate}" "${output_dir}/oracle.clear.pgm")"
  awk -v global="${global_rmse}" -v silhouette="${silhouette_rmse}" \
      -v clear="${clear_rmse}" \
      'BEGIN { exit !(global<=0.005 && silhouette<=0.01 && clear<=0.005) }'
  magick compare "${output_dir}/oracle.ppm" \
    "${output_dir}/${candidate}.ppm" \
    "${output_dir}/${candidate}-difference.png" 2>/dev/null || true
  report="$(jq -c --arg candidate "${candidate}" \
    --argjson global "${global_rmse}" \
    --argjson silhouette "${silhouette_rmse}" \
    --argjson clear "${clear_rmse}" \
    '.+[{candidate:$candidate,global_rmse:$global,
      silhouette_rmse:$silhouette,clear_rmse:$clear}]' <<<"${report}")"
done

magick montage "${output_dir}/oracle.ppm" \
  "${output_dir}/deterministic-half.ppm" \
  "${output_dir}/temporal-divisor-2.ppm" \
  "${output_dir}/temporal-divisor-3.ppm" \
  "${output_dir}/temporal-divisor-4.ppm" -thumbnail 1280x800 -tile 5x1 \
  -geometry +8+24 -background white -set label '%t' \
  "${output_dir}/comparison.png"

jq -n --arg window_size "${window_size}" \
  --arg framebuffer_size "${oracle_dimensions}" --argjson candidates "${report}" \
  '{event:"atmosphere_reconstruction_qualification", status:"pass",
    window_size:$window_size, framebuffer_size:$framebuffer_size,
    candidates:$candidates,
    thresholds:{global_rmse:0.005,silhouette_rmse:0.01,clear_rmse:0.005}}' |
  tee "${output_dir}/qualification.json"
