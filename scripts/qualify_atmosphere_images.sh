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

# The compact production terrain must remain visibly enclosed by atmosphere
# from orbit.  The capture reports only clear pixels immediately outside the
# rendered geometry silhouette, so this cannot be satisfied by bright terrain.
if [[ " ${transports_text} " == *" faithful-hillaire "* &&
      " ${qualities_text} " == *" default "* ]]; then
  orbit_evidence="${output_dir}/faithful-hillaire-default-orbit.jsonl"
  orbit_capture="$(grep '"event":"gpu_atmosphere_capture"' \
    "${orbit_evidence}")"
  jq -e '.outer_limb.sampled_pixels > 1000 and
         .outer_limb.black_fraction < 0.25 and
         .outer_limb.blue_fraction > 0.65 and
         .outer_limb.mean[2] > .outer_limb.mean[1] and
         .outer_limb.mean[1] > .outer_limb.mean[0]' \
    <<<"${orbit_capture}" >/dev/null

  # The compact planet's shallow Mie layer projects to a sub-pixel limb in
  # orbit. Sampling it only through the angular sky-view LUT made the yellow
  # band fade under tiny rotations. Exercise the high-altitude primary ray at
  # three nearby poses and require the clear outer limb to remain stable.
  orbital_motion_evidence=()
  for pitch in -88.00 -87.98 -87.96; do
    suffix="${pitch//-/m}"
    suffix="${suffix//./p}"
    run_case faithful-hillaire default "orbit-motion-${suffix}" \
      "0.5,50000.5,0.5" 180 "${pitch}" -103.1324 5
    orbital_motion_evidence+=(
      "${output_dir}/faithful-hillaire-default-orbit-motion-${suffix}.jsonl")
  done
  for altitude in 49800.5 50200.5; do
    suffix="${altitude//./p}"
    run_case faithful-hillaire default "orbit-ascent-${suffix}" \
      "0.5,${altitude},0.5" 180 -88 -103.1324 5
    orbital_motion_evidence+=(
      "${output_dir}/faithful-hillaire-default-orbit-ascent-${suffix}.jsonl")
  done
  jq -s -e '
    map(select(.event == "gpu_atmosphere_capture") | .outer_limb) as $m |
    ($m | map(.luminance_mean) | max-min) < 0.002 and
    ($m | map(.black_fraction) | max-min) < 0.01 and
    ($m | map(.blue_fraction) | max-min) < 0.02 and
    ($m | map(.sampled_pixels) | min) > 1000
  ' "${orbital_motion_evidence[@]}" >/dev/null

  # A retained full-sky table used to make the forward Mie lobe lose roughly
  # one quarter of its warm intensity over this small, real gameplay camera
  # motion.  Measure only clear sky in the upper two thirds of the image so
  # changing terrain coverage cannot satisfy or fail the regression.
  run_case faithful-hillaire default mie-motion-a \
    "-41.229,117.570,285.110" 170.0 -23.5 -103 5
  run_case faithful-hillaire default mie-motion-b \
    "-49.217,132.046,318.756" 166.5 -22.7 -103 5
  python3 - "${output_dir}" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])

def pixels(path):
    header = path.read_bytes().split(b'\n', 3)
    width, height = map(int, header[1].split())
    return width, height, header[3]

statistics = []
for suffix in ('a', 'b'):
    image = root / f'faithful-hillaire-default-mie-motion-{suffix}.ppm'
    width, height, rgb = pixels(image)
    _, _, clear = pixels(image.with_suffix('.clear.pgm'))
    warm = []
    for index, visible in enumerate(clear):
        if visible and index // width < 2 * height // 3:
            red, _, blue = rgb[3 * index:3 * index + 3]
            warm.append(max(0, red - blue))
    if len(warm) < 10000:
        raise SystemExit('Mie motion regression has too little clear sky')
    warm.sort()
    statistics.append((warm[int(0.99 * (len(warm) - 1))], warm[-1]))

if abs(statistics[0][0] - statistics[1][0]) > 8:
    raise SystemExit(f'Mie warm percentile changed across motion: {statistics}')
if abs(statistics[0][1] - statistics[1][1]) > 5:
    raise SystemExit(f'Mie warm peak changed across motion: {statistics}')
PY

  # Keep the corresponding aerial-perspective lobe stable over opaque
  # terrain.  This smaller translation has fixed orientation and deliberately
  # straddles a retained aerial-volume update boundary.
  run_case faithful-hillaire default mie-terrain-motion-a \
    "-117.309,410.539,963.202" 165.7 -21.8 -103 5
  run_case faithful-hillaire default mie-terrain-motion-b \
    "-116.699,409.551,960.810" 165.7 -21.8 -103 5
  python3 - "${output_dir}" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])

def pixels(path):
    header = path.read_bytes().split(b'\n', 3)
    width, height = map(int, header[1].split())
    return width, height, header[3]

statistics = []
for suffix in ('a', 'b'):
    image = root / f'faithful-hillaire-default-mie-terrain-motion-{suffix}.ppm'
    width, height, rgb = pixels(image)
    _, _, depth = pixels(image.with_suffix('.depth.pgm'))
    warm = []
    for index, geometry in enumerate(depth):
        if geometry and index // width < 2 * height // 3:
            red, _, blue = rgb[3 * index:3 * index + 3]
            warm.append(max(0, red - blue))
    if len(warm) < 10000:
        raise SystemExit('Mie terrain-motion regression has too little geometry')
    warm.sort()
    last = len(warm) - 1
    statistics.append((warm[int(0.95 * last)], warm[int(0.99 * last)], warm[-1]))

if any(abs(first - second) > 2
       for first, second in zip(statistics[0], statistics[1])):
    raise SystemExit(f'Mie terrain haze changed across motion: {statistics}')
PY

  # Cover clear sky well outside the forward Mie cone.  Final sky pixels must
  # not fall back to the retained, position-quantized table in this region.
  run_case faithful-hillaire default mie-wide-sky-motion-a \
    "-553.996,1034.270,2332.481" 151.9 -28.1 -103 5
  run_case faithful-hillaire default mie-wide-sky-motion-b \
    "-555.054,1035.471,2334.463" 151.9 -28.1 -103 5
  python3 - "${output_dir}" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])

def pixels(path):
    header = path.read_bytes().split(b'\n', 3)
    width, height = map(int, header[1].split())
    return width, height, header[3]

means = []
for suffix in ('a', 'b'):
    image = root / f'faithful-hillaire-default-mie-wide-sky-motion-{suffix}.ppm'
    width, height, rgb = pixels(image)
    _, _, clear = pixels(image.with_suffix('.clear.pgm'))
    totals = [0, 0, 0]
    count = 0
    for y in range(height // 2):
        for x in range(width // 2):
            index = y * width + x
            if not clear[index]:
                continue
            for channel in range(3):
                totals[channel] += rgb[3 * index + channel]
            count += 1
    if count < 10000:
        raise SystemExit('wide-sky Mie regression has too little clear sky')
    means.append(tuple(value / count for value in totals))

if any(abs(first - second) > 0.1
       for first, second in zip(means[0], means[1])):
    raise SystemExit(f'wide clear sky changed across motion: {means}')
PY

  # Exercise a view whose visible terrain extends beyond the old
  # current-ray Mie cone.  Both clear sky and opaque aerial perspective must
  # stay stable now that final faithful pixels share one integration path.
  run_case faithful-hillaire default mie-all-current-motion-a \
    "-461.473,570.474,1591.811" 163.2 -19.2 -103 5
  run_case faithful-hillaire default mie-all-current-motion-b \
    "-462.461,571.668,1595.091" 163.2 -19.2 -103 5
  python3 - "${output_dir}" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])

def pixels(path):
    header = path.read_bytes().split(b'\n', 3)
    width, height = map(int, header[1].split())
    return width, height, header[3]

statistics = []
for suffix in ('a', 'b'):
    image = root / f'faithful-hillaire-default-mie-all-current-motion-{suffix}.ppm'
    width, height, rgb = pixels(image)
    sample = []
    for extension in ('.clear.pgm', '.depth.pgm'):
        _, _, mask = pixels(image.with_suffix(extension))
        warm = []
        for index, selected in enumerate(mask):
            if selected and index // width < 2 * height // 3:
                red, _, blue = rgb[3 * index:3 * index + 3]
                warm.append(max(0, red - blue))
        if len(warm) < 10000:
            raise SystemExit('all-current Mie regression has too few samples')
        warm.sort()
        last = len(warm) - 1
        sample.extend((warm[int(0.95 * last)], warm[int(0.99 * last)], warm[-1]))
    statistics.append(sample)

if any(abs(first - second) > 2
       for first, second in zip(statistics[0], statistics[1])):
    raise SystemExit(f'all-current Mie output changed across motion: {statistics}')
PY
fi

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
