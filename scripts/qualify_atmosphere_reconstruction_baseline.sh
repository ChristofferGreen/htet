#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/tetra_world}"
output_dir="${2:-${repo_root}/build/atmosphere-reconstruction-baseline}"
window_size="${TETRA_ATMOSPHERE_BASELINE_WINDOW_SIZE:-2560x1600}"

if [[ ! -x "${binary}" ]]; then
  echo "release tetra_world executable is missing: ${binary}" >&2
  exit 2
fi
mkdir -p "${output_dir}"

common=(
  --window-size="${window_size}" --free-fly --surface-edges-off
  --atmosphere-preset=gameplay-planet --atmosphere-quality=default
  --atmosphere-transport=faithful-hillaire
  --atmosphere-shadow-integrator=minmax-segments
  --atmosphere-shadow-filter=physical-footprint --exposure-ev=-0.62
)

capture() {
  local name="$1"
  shift
  "${binary}" "${common[@]}" "$@" \
    --gpu-atmosphere-capture="${output_dir}/${name}.ppm" \
    >"${output_dir}/${name}.jsonl"
  jq -e 'select(.event=="gpu_atmosphere_capture") |
    .analysis.black_fraction < 1 and .analysis.clipped_fraction < 1' \
    "${output_dir}/${name}.jsonl" >/dev/null
}

mountain=(
  --camera-feet=0.5,0.5,0.78 --camera-yaw-degrees=137.6
  --sun-azimuth-degrees=-49 --sun-elevation-degrees=5
)
for pitch in -3.90 -3.85 -3.80 -3.75 -3.70; do
  suffix="${pitch#-}"
  suffix="${suffix/./p}"
  capture "mountain-pitch-${suffix}" "${mountain[@]}" \
    --camera-pitch-degrees="${pitch}"
done

# Small translations around the exact reported pose. The sequence is retained
# as images rather than collapsed to one scalar so detached silhouettes,
# temporal popping, and changes hidden by mean luminance remain inspectable.
translations=(
  "0.500,0.500,0.780"
  "0.505,0.495,0.780"
  "0.510,0.490,0.780"
  "0.515,0.485,0.780"
  "0.520,0.480,0.780"
)
for index in "${!translations[@]}"; do
  capture "mountain-translation-${index}" \
    --camera-feet="${translations[index]}" --camera-yaw-degrees=137.6 \
    --camera-pitch-degrees=-3.8 --sun-azimuth-degrees=-49 \
    --sun-elevation-degrees=5
done

for pitch in -88.00 -87.98 -87.96; do
  suffix="${pitch#-}"
  suffix="${suffix/./p}"
  capture "orbit-pitch-${suffix}" --camera-feet=0.5,50000.5,0.5 \
    --camera-yaw-degrees=180 --camera-pitch-degrees="${pitch}" \
    --sun-azimuth-degrees=-103.1324 --sun-elevation-degrees=5
done

capture "native-oracle-mountain" "${mountain[@]}" \
  --camera-pitch-degrees=-3.8 \
  --atmosphere-rendering-method=native-screen-oracle

"${binary}" "${common[@]}" "${mountain[@]}" \
  --camera-pitch-degrees=-3.8 --gpu-atmosphere-benchmark \
  >"${output_dir}/benchmark-current.jsonl"
jq -e 'select(.event=="gpu_atmosphere_benchmark") |
  .atmosphere.median_ms >= 0 and .composite.median_ms >= 0' \
  "${output_dir}/benchmark-current.jsonl" >/dev/null
"${binary}" "${common[@]}" "${mountain[@]}" \
  --camera-pitch-degrees=-3.8 \
  --atmosphere-rendering-method=native-screen-oracle \
  --gpu-atmosphere-benchmark >"${output_dir}/benchmark-native-oracle.jsonl"
jq -e 'select(.event=="gpu_atmosphere_benchmark") |
  .atmosphere.median_ms >= 0 and .composite.median_ms >= 0' \
  "${output_dir}/benchmark-native-oracle.jsonl" >/dev/null

python3 - "${output_dir}" "${window_size}" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

root=Path(sys.argv[1])
captures=[]
for image in sorted(root.glob('*.ppm')):
    if image.stem=='native-oracle-preview':
        continue
    evidence=image.with_suffix('.jsonl')
    record=json.loads(evidence.read_text().splitlines()[-1])
    captures.append({
        'name':image.stem,
        'width':record['width'],
        'height':record['height'],
        'rendering_method':record['rendering_method'],
        'rgb_hash':record['rgb_hash'],
        'sha256':hashlib.sha256(image.read_bytes()).hexdigest(),
        'analysis':record['analysis'],
        'sun_pixel':record.get('sun_pixel'),
        'sun_centre_geometry_occluded':record.get(
            'sun_centre_geometry_occluded'),
    })
benchmarks={}
for path in sorted(root.glob('benchmark-*.jsonl')):
    benchmarks[path.stem.removeprefix('benchmark-')]=json.loads(
        path.read_text().splitlines()[-1])
manifest={
    'event':'atmosphere_reconstruction_baseline',
    'window_size':sys.argv[2],
    'capture_count':len(captures),
    'captures':captures,
    'benchmarks':benchmarks,
}
(root/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest))
PY

if command -v magick >/dev/null 2>&1; then
  magick montage "${output_dir}"/*.ppm -thumbnail 960x600 -tile 3x \
    -geometry +8+24 -background white -set label '%t' \
    "${output_dir}/contact-sheet.png"
fi
