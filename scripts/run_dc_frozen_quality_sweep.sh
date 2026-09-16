#!/usr/bin/env bash
# Fresh DC/curtain/core -> frozen-facet scaffold-quality sweep.
#
# This is an offline oracle only.  It deliberately creates no reusable shell:
# every threshold starts with the same exporter input and invokes the
# caller-supplied mesher afresh.  TetGen's AGPL/commercial license means this
# must not be made a runtime dependency. The exported PLC intentionally leaves
# the normal-offset lower DC front as a boundary, so this does not qualify a
# complete terrain volume.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 4 ]]; then
  printf 'usage: %s /absolute/path/to/tetgen [resolution [phase_x phase_y]]\n' "$0" >&2
  exit 2
fi

tetgen_bin=$1
resolution=${2:-8}
phase_x=${3:-0.23}
phase_y=${4:-0.41}
if [[ ! -x "$tetgen_bin" ]]; then
  printf 'TetGen executable is not runnable: %s\n' "$tetgen_bin" >&2
  exit 2
fi

repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-frozen-quality.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT

c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/export_shell.cpp" -o "$scratch_dir/export_shell"
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/verify_shell.cpp" -o "$scratch_dir/verify_shell"

for ratio in 1.01 1.1 1.2 1.4 1.8; do
  # TetGen treats the final dot in a path as its input extension, so the
  # numerical quality ratio cannot appear verbatim in the output prefix.
  ratio_key=${ratio//./_}
  prefix="$scratch_dir/q${ratio_key}"
  "$scratch_dir/export_shell" "$prefix.poly" "$resolution" "$phase_x" "$phase_y" >/dev/null
  "$tetgen_bin" -pYq"$ratio" "$prefix.poly" >/dev/null
  printf 'q%s ' "$ratio"
  "$scratch_dir/verify_shell" "$prefix" "$resolution"
done
