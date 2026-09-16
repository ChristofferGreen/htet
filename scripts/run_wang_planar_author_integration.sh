#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd "${script_dir}/.." && pwd)
build_dir="${repo_dir}/build-release"

cmake -S "${repo_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
cmake --build "${build_dir}" --target wang_planar_probe -j4 >/dev/null

result_file=$(mktemp "${TMPDIR:-/tmp}/wang-planar-author.XXXXXX")
trap 'rm -f "${result_file}"' EXIT
"${build_dir}/wang_planar_probe" >"${result_file}" 2>&1
summary=$(rg '^accepted=' "${result_file}")

if [[ "${summary}" != *'accepted=1'* ||
      "${summary}" != *'missing_edges=0'* ||
      "${summary}" != *'missing_facets=0'* ||
      "${summary}" != *'audit_no_boundary_steiner=1'* ||
      "${summary}" != *'duplicate_cells=0'* ||
      "${summary}" != *'degenerate_cells=0'* ||
      "${summary}" != *'nonmanifold_faces=0'* ]]; then
  cat "${result_file}" >&2
  exit 1
fi

printf '%s\n' "${summary}"
