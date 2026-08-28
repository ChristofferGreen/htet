#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cache_dir="${TETRA_ATMOSPHERE_REFERENCE_CACHE:-${repo_root}/build/atmosphere-reference-cache}"
output_dir="${1:-${repo_root}/build/atmosphere-references}"
prague_commit="2385c912e9051c1258013ff8c3ce2e19e10fb917"
prague_dataset_sha="76bd619dc6dfcbc900c2996436dd6cf68197c03e3982fe5580f2b109ce1c71c2"
dataset="${cache_dir}/PragueSkyModelDataset.dat"

mkdir -p "${cache_dir}" "${output_dir}"

sha256() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    sha256sum "$1" | awk '{print $1}'
  fi
}

download_checked() {
  local url="$1" destination="$2" expected="$3"
  if [[ ! -f "${destination}" ]]; then
    curl --fail --location --silent --show-error \
      "${url}" --output "${destination}"
  fi
  local actual
  actual="$(sha256 "${destination}")"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "reference hash mismatch: ${destination}" >&2
    echo "expected ${expected}, got ${actual}" >&2
    exit 3
  fi
}

bruneton_base="https://ebruneton.github.io/precomputed_atmospheric_scattering/atmosphere/reference"
download_checked "${bruneton_base}/RadianceSeparateTextures2.png" \
  "${output_dir}/bruneton-cpu-noon-radiance.png" \
  "b3757c0114cf546aa24dad49ebaa02f4e7406067be39518b75b8e8c24bb84c23"
download_checked "${bruneton_base}/RadianceCombineTexturesSunSet2.png" \
  "${output_dir}/bruneton-cpu-sunset-radiance.png" \
  "b59ce1517a476a6dd9e45956ef33c4cba70962f4920bb89a5e665d72ca83a379"
download_checked "${bruneton_base}/LuminanceSeparateTexturesConstantAlbedo2.png" \
  "${output_dir}/bruneton-cpu-noon-luminance.png" \
  "dd3b21b81f9083e44108bc8576cd00b9bb2d240fb936cf675b759d4ce2b9730d"
download_checked "${bruneton_base}/LuminanceCombineTexturesConstantAlbedoSunSet2.png" \
  "${output_dir}/bruneton-cpu-sunset-luminance.png" \
  "c369a5da2603c7a10c9bbef123cb97fde3c58f605f99f3f273716916b18ec8c7"

prague_source="${cache_dir}/prague-sky-model"
if [[ ! -d "${prague_source}/.git" ]]; then
  git clone --quiet https://github.com/PetrVevoda/pragueskymodel.git \
    "${prague_source}"
fi
git -C "${prague_source}" fetch --quiet origin "${prague_commit}"
git -C "${prague_source}" checkout --quiet --detach "${prague_commit}"

download_checked \
  "https://drive.usercontent.google.com/download?id=1IflyFZTJxC_N298yXq_2GK4ycIsVJZk6&export=download&confirm=t" \
  "${dataset}" "${prague_dataset_sha}"

reference_build="${cache_dir}/prague-build"
mkdir -p "${reference_build}"
cc="${CC:-cc}"
cxx="${CXX:-c++}"
"${cc}" -std=c99 -O2 -I"${prague_source}/thirdparty/miniz" \
  -c "${prague_source}/thirdparty/miniz/miniz.c" \
  -o "${reference_build}/miniz.o"
# Do not define NDEBUG: the reference header then uses its deterministic serial
# loop and has no platform TBB dependency.
"${cxx}" -std=c++17 -O2 \
  -I"${prague_source}/thirdparty" \
  -I"${prague_source}/thirdparty/miniz" \
  -I"${prague_source}/thirdparty/tinyexr" \
  "${prague_source}/src/PragueSkyModel.cpp" \
  "${prague_source}/src/PragueSkyModelTestCli.cpp" \
  "${reference_build}/miniz.o" \
  -o "${reference_build}/PragueSkyModelCli"

render_prague() {
  local name="$1" elevation="$2"
  "${reference_build}/PragueSkyModelCli" \
    -dat "${dataset}" -alb 0.32 -alt 0 -azi 0 -ele "${elevation}" \
    -cam 1 -chn 0 -mod 0 -res 128 -vis 59.4 \
    -out "${output_dir}/prague-${name}.exr"
}
render_prague noon 60
render_prague sunset 1

printf 'source\tidentity\n'
printf 'bruneton\t%s\n' '34f14e745cff948f4ca3157d1b62a445ffa7286f'
printf 'prague\t%s\n' "${prague_commit}"
printf 'prague-dataset-sha256\t%s\n' "${prague_dataset_sha}"
printf 'prague-noon-exr-sha256\t%s\n' \
  "$(sha256 "${output_dir}/prague-noon.exr")"
printf 'prague-sunset-exr-sha256\t%s\n' \
  "$(sha256 "${output_dir}/prague-sunset.exr")"
