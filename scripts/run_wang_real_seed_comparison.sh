#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd "${script_dir}/.." && pwd)
author_build="${repo_dir}/build-wang-reference"
prototype_build="${repo_dir}/build-release"

if command -v g++-14 >/dev/null 2>&1; then
  compiler=$(command -v g++-14)
elif [[ -x /opt/homebrew/bin/g++-14 ]]; then
  compiler=/opt/homebrew/bin/g++-14
else
  echo "GCC 14 is required for the pinned author differential" >&2
  exit 1
fi

cmake -S "${repo_dir}/tools/wang_reference_harness" -B "${author_build}" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER="${compiler}" >/dev/null
cmake --build "${author_build}" --target wang_author_reference_probe -j4 >/dev/null
cmake -S "${repo_dir}" -B "${prototype_build}" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DTETRA_BUILD_WANG_AUTHOR_ORACLE=ON >/dev/null
cmake --build "${prototype_build}" --target wang_fixture_surface_export \
  wang_prototype_reference_probe -j4 >/dev/null

work=$(mktemp -d "${TMPDIR:-/tmp}/wang-real-seed.XXXXXX")
trap 'rm -rf "${work}"' EXIT
for noise in 0 0.075; do
  label="planar"; [[ "${noise}" == "0" ]] || label="noisy"
  surface="${work}/${label}.vtk"
  author="${work}/${label}.author"
  owned="${work}/${label}.owned"
  "${prototype_build}/wang_fixture_surface_export" "${surface}" 5 "${noise}"
  "${author_build}/wang_author_reference_probe" seed_file "${surface}" "${author}"
  "${prototype_build}/wang_prototype_reference_probe" seed_file "${surface}" "${owned}"
  if ! "${script_dir}/compare_wang_seed_stages.sh" "${author}" "${owned}"; then
    echo "Wang real seed comparison diverged for ${label}" >&2
    exit 1
  fi
done
echo "Wang real seed comparison passed"
