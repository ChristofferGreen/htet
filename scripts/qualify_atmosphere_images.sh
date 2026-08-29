#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/tetra_world}"
output_dir="${2:-${repo_root}/build/atmosphere-qualification}"
window_size="${TETRA_ATMOSPHERE_WINDOW_SIZE:-960x540}"
transports_text="${TETRA_ATMOSPHERE_TRANSPORTS:-qualified-baseline faithful-hillaire}"
qualities_text="${TETRA_ATMOSPHERE_QUALITIES:-default}"

if [[ ! -x "${binary}" ]]; then
  echo "release tetra_world executable is missing: ${binary}" >&2
  exit 2
fi
mkdir -p "${output_dir}"

run_cpu_probe() {
  local name="$1" altitude="$2" view_zenith="$3" sun_zenith="$4"
  "${binary}" --atmosphere-check gameplay-planet \
    "${altitude}" "${view_zenith}" "${sun_zenith}" |
    tee "${output_dir}/cpu-${name}.jsonl"
}

run_cpu_probe ground 2.2 104.3239 85
run_cpu_probe noon 2.2 104.3239 30
run_cpu_probe sunset 2.2 98 89
run_cpu_probe mountain-shadow 2.2 96 85
run_cpu_probe flight 1000 95 65
run_cpu_probe limb 50000 128.5 80
run_cpu_probe orbit 250000 178 55
run_cpu_probe terminator 250000 178 90

run_case() {
  local transport="$1" quality="$2" name="$3" camera="$4" yaw="$5"
  local pitch="$6" sun_azimuth="$7" sun_elevation="$8"
  local image="${output_dir}/${transport}-${quality}-${name}.ppm"
  local evidence="${output_dir}/${transport}-${quality}-${name}.jsonl"
  local probe=()
  if [[ "${transport}" == "faithful-hillaire" ]]; then
    probe=(--gpu-atmosphere-probe)
  fi
  "${binary}" \
    --window-size="${window_size}" \
    --free-fly \
    --atmosphere-preset=gameplay-planet \
    --atmosphere-quality="${quality}" \
    --atmosphere-transport="${transport}" \
    --exposure-ev=-0.62 \
    --camera-feet="${camera}" \
    --camera-yaw-degrees="${yaw}" \
    --camera-pitch-degrees="${pitch}" \
    --sun-azimuth-degrees="${sun_azimuth}" \
    --sun-elevation-degrees="${sun_elevation}" \
    --gpu-atmosphere-benchmark \
    "${probe[@]}" \
    --gpu-atmosphere-capture="${image}" | tee "${evidence}"
}

# Coordinates are world units (10 metres per unit).  The nominal north-pole
# terrain surface is y=0.5 and the compact planet centre is y=-19999.5.
read -r -a transports <<< "${transports_text}"
read -r -a qualities <<< "${qualities_text}"
for quality in "${qualities[@]}"; do
  if [[ "${quality}" != "low" && "${quality}" != "default" &&
        "${quality}" != "high" ]]; then
    echo "unknown atmosphere quality in TETRA_ATMOSPHERE_QUALITIES: ${quality}" >&2
    exit 2
  fi
  for transport in "${transports[@]}"; do
    if [[ "${transport}" != "qualified-baseline" &&
          "${transport}" != "faithful-hillaire" ]]; then
      echo "unknown atmosphere transport in TETRA_ATMOSPHERE_TRANSPORTS: ${transport}" >&2
      exit 2
    fi
    run_case "${transport}" "${quality}" ground "0.5,0.72,0.78" 180 -14.3239 -103.1324 5
    run_case "${transport}" "${quality}" noon "0.5,0.72,0.78" 180 -14.3239 -103.1324 60
    run_case "${transport}" "${quality}" sunset "0.5,0.72,0.78" 180 -8 -103.1324 1
    run_case "${transport}" "${quality}" mountain-shadow "0.5,0.72,0.78" 180 -6 -75 5
    run_case "${transport}" "${quality}" flight "0.5,100.5,0.78" 180 -5 -103.1324 25
    run_case "${transport}" "${quality}" limb "0.5,5000.5,0.5" 180 -38.5 -103.1324 10
    run_case "${transport}" "${quality}" orbit "0.5,25000.5,0.5" 180 -88 -103.1324 35
    run_case "${transport}" "${quality}" terminator "0.5,25000.5,0.5" 180 -88 90 0
  done
done

# The presentation matrix uses the compact gameplay planet, but transport
# endpoint behavior also depends on each preset's radius and atmosphere height.
# Probe every other shipped physical preset so large-radius float boundary
# regressions cannot hide behind the gameplay scale.
if [[ " ${transports_text} " == *" faithful-hillaire "* &&
      " ${qualities_text} " == *" default "* ]]; then
  for preset in earth mars-like dense-haze nearly-airless; do
    "${binary}" \
      --window-size="${window_size}" \
      --free-fly \
      --atmosphere-preset="${preset}" \
      --atmosphere-quality=default \
      --atmosphere-transport=faithful-hillaire \
      --camera-feet=0.5,0.72,0.78 \
      --gpu-atmosphere-probe |
      tee "${output_dir}/faithful-hillaire-default-preset-${preset}.jsonl"
  done
fi

# H7 uses a deterministic ridge-facing view.  At five degrees the sun lies
# behind the left ridge and the diagnostic must contain both occluded and lit
# rays.  At noon no above-ground atmospheric sample should be terrain-shadowed.
# These are deliberately separate from the older presentation-oriented
# mountain view, whose sun sits beside rather than behind the ridge.
if [[ " ${transports_text} " == *" faithful-hillaire "* &&
      " ${qualities_text} " == *" default "* ]]; then
  run_shadow_oracle() {
    local name="$1" elevation="$2" expectation="$3"
    local image="${output_dir}/faithful-hillaire-default-${name}.ppm"
    local evidence="${output_dir}/faithful-hillaire-default-${name}.jsonl"
    "${binary}" \
      --window-size="${window_size}" \
      --free-fly \
      --atmosphere-preset=gameplay-planet \
      --atmosphere-quality=default \
      --atmosphere-transport=faithful-hillaire \
      --atmosphere-debug=11 \
      --exposure-ev=-0.62 \
      --camera-feet=0.5,0.72,0.78 \
      --camera-yaw-degrees=180 \
      --camera-pitch-degrees=-6 \
      --sun-azimuth-degrees=-45 \
      --sun-elevation-degrees="${elevation}" \
      --gpu-atmosphere-capture="${image}" | tee "${evidence}"
    local capture
    capture="$(grep '"event":"gpu_atmosphere_capture"' "${evidence}")"
    if [[ "${expectation}" == "mixed" ]]; then
      jq -e '.analysis.maximum[0] > 0 and
             .analysis.black_fraction > 0.05 and
             .analysis.black_fraction < 0.95' <<<"${capture}" >/dev/null
    else
      jq -e '.analysis.maximum == [0,0,0] and
             .analysis.black_fraction == 1' <<<"${capture}" >/dev/null
    fi
  }
  run_shadow_oracle ridge-shadow-coverage 5 mixed
  run_shadow_oracle ridge-noon-guard 60 clear
fi
