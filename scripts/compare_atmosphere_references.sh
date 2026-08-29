#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${1:-${repo_root}/build/release/src/tetra_viewer/tetra_world}"
output_dir="${2:-${repo_root}/build/atmosphere-reference-comparison}"
cache_dir="${TETRA_ATMOSPHERE_REFERENCE_CACHE:-${repo_root}/build/atmosphere-reference-cache}"
reference_dir="${output_dir}/references"
mkdir -p "${output_dir}"

if [[ ! -x "${binary}" ]]; then
  echo "release tetra_world executable is missing: ${binary}" >&2
  exit 2
fi
if ! command -v jq >/dev/null 2>&1; then
  echo "jq is required for atmosphere reference comparison" >&2
  exit 2
fi

"${repo_root}/scripts/prepare_atmosphere_references.sh" "${reference_dir}" \
  >"${output_dir}/reference-provenance.tsv"
prague_probe="${cache_dir}/prague-build/prague_atmosphere_probe"
bruneton_probe="${cache_dir}/bruneton_atmosphere_probe"
bruneton_probe_cache="${cache_dir}/bruneton-probe-cache/"
dataset="${cache_dir}/PragueSkyModelDataset.dat"

printf 'case\tsun_elevation\tview_elevation\tview_azimuth\trenderer_r\trenderer_g\trenderer_b\tbruneton_r\tbruneton_g\tbruneton_b\tbruneton_maximum_error\tbruneton_l1_error\tprague_r\tprague_g\tprague_b\tprague_maximum_error\tprague_l1_error\n' \
  >"${output_dir}/matched-sky-chromaticity.tsv"

# The Prague implementation applies a documented 50 m safety altitude at
# ground level. With the world's 10 m units and 0.145-unit eye height, a feet
# height of 5.355 places this renderer's probe at the same physical altitude.
# Prague azimuth zero maps to renderer yaw 90; azimuth 90 maps to yaw zero.
cases=(
  'noon-toward:60:30:0:90:blue'
  'noon-cross:60:10:90:0:blue'
  'noon-zenith:60:85:0:90:blue'
  'sunset-toward:1:5:0:90:warm'
  'sunset-cross:1:5:90:0:warm'
  'sunset-zenith:1:85:0:90:blue'
)

for case_data in "${cases[@]}"; do
  IFS=: read -r name sun view azimuth yaw ordering <<<"${case_data}"
  renderer_json="${output_dir}/renderer-${name}.jsonl"
  bruneton_json="${output_dir}/bruneton-${name}.json"
  bruneton_raw="${output_dir}/bruneton-${name}.raw"
  prague_json="${output_dir}/prague-${name}.json"
  "${binary}" \
    --window-size=320x180 \
    --free-fly \
    --atmosphere-preset=earth \
    --atmosphere-quality=default \
    --atmosphere-transport=faithful-hillaire \
    --camera-feet=0.5,5.355,0.5 \
    --camera-yaw-degrees="${yaw}" \
    --camera-pitch-degrees="${view}" \
    --sun-azimuth-degrees=0 \
    --sun-elevation-degrees="${sun}" \
    --gpu-atmosphere-probe >"${renderer_json}"
  "${prague_probe}" "${dataset}" "${sun}" "${view}" "${azimuth}" 0.1 \
    >"${prague_json}"
  "${bruneton_probe}" "${bruneton_probe_cache}" \
    "${sun}" "${view}" "${azimuth}" 4 >"${bruneton_raw}"
  grep -o '{.*}' "${bruneton_raw}" | tail -n 1 >"${bruneton_json}"

  jq -e 'select(.event=="gpu_atmosphere_probe") | .status=="pass"' \
    "${renderer_json}" >/dev/null
  renderer_rgb="$(jq -c \
    '[.comparisons[] | select(.stage=="full_sky_lookup") | .actual[0:3]][0]' \
    "${renderer_json}")"
  renderer_chroma="$(jq -cn --argjson rgb "${renderer_rgb}" \
    '$rgb as $v | ($v|add) as $sum | [$v[]/$sum]')"
  bruneton_chroma="$(jq -c '.chromaticity' "${bruneton_json}")"
  prague_chroma="$(jq -c '.chromaticity' "${prague_json}")"
  bruneton_errors="$(jq -cn --argjson renderer "${renderer_chroma}" \
    --argjson reference "${bruneton_chroma}" \
    '[range(0;3) as $i | (($renderer[$i]-$reference[$i]) |
      if . < 0 then -. else . end)] | {maximum:max,l1:add}')"
  prague_errors="$(jq -cn --argjson renderer "${renderer_chroma}" \
    --argjson prague "${prague_chroma}" \
    '[range(0;3) as $i | (($renderer[$i]-$prague[$i]) |
      if . < 0 then -. else . end)] | {maximum:max,l1:add}')"

  # Three-band realtime coefficients cannot match Prague's 55-channel spectral
  # integration absolutely. The matched-domain gate instead requires stable
  # chromaticity and the physically diagnostic channel ordering. These bounds
  # were selected from all six directions, not from one favorable pixel.
  jq -en --argjson errors "${bruneton_errors}" \
    '$errors.maximum <= 0.015 and $errors.l1 <= 0.03' >/dev/null
  jq -en --argjson errors "${prague_errors}" \
    '$errors.maximum <= 0.16 and $errors.l1 <= 0.32' >/dev/null
  if [[ "${ordering}" == "blue" ]]; then
    jq -en --argjson rgb "${renderer_chroma}" \
      '$rgb[2] > $rgb[1] and $rgb[1] > $rgb[0]' >/dev/null
    jq -en --argjson rgb "${prague_chroma}" \
      '$rgb[2] > $rgb[1] and $rgb[1] > $rgb[0]' >/dev/null
    jq -en --argjson rgb "${bruneton_chroma}" \
      '$rgb[2] > $rgb[1] and $rgb[1] > $rgb[0]' >/dev/null
  else
    jq -en --argjson rgb "${renderer_chroma}" \
      '$rgb[0] > $rgb[1] and $rgb[1] > $rgb[2]' >/dev/null
    jq -en --argjson rgb "${prague_chroma}" \
      '$rgb[0] > $rgb[1] and $rgb[1] > $rgb[2]' >/dev/null
    jq -en --argjson rgb "${bruneton_chroma}" \
      '$rgb[0] > $rgb[1] and $rgb[1] > $rgb[2]' >/dev/null
  fi

  jq -nr --arg name "${name}" --arg sun "${sun}" --arg view "${view}" \
    --arg azimuth "${azimuth}" --argjson renderer "${renderer_chroma}" \
    --argjson bruneton "${bruneton_chroma}" \
    --argjson bruneton_errors "${bruneton_errors}" \
    --argjson prague "${prague_chroma}" \
    --argjson prague_errors "${prague_errors}" \
    '[$name,$sun,$view,$azimuth,$renderer[0],$renderer[1],$renderer[2],
      $bruneton[0],$bruneton[1],$bruneton[2],
      $bruneton_errors.maximum,$bruneton_errors.l1,
      $prague[0],$prague[1],$prague[2],
      $prague_errors.maximum,$prague_errors.l1] | @tsv' \
    >>"${output_dir}/matched-sky-chromaticity.tsv"
done

echo "matched atmosphere reference comparison passed"
