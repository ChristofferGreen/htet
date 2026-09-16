#!/usr/bin/env bash
# Run the frozen-DC -> constrained shell -> retained-grid-core research oracle.
#
# The mesher is intentionally supplied by the caller. TetGen is an external
# research dependency with AGPL/commercial licensing, not a project/runtime or
# GPU dependency. The exporter and verifier compile against the same probe
# geometry that the inspector uses, then the verifier fails closed unless the
# frozen outer facets and retained regular core both survive unchanged.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 5 ]]; then
  printf 'usage: %s /absolute/path/to/tetgen [resolution [phase_x phase_y [--require-quality]]]\n' "$0" >&2
  exit 2
fi

tetgen_bin=$1
resolution=${2:-8}
phase_x=${3:-0.23}
phase_y=${4:-0.41}
quality_option=${5:-}
if [[ -n "$quality_option" && "$quality_option" != "--require-quality" ]]; then
  printf 'unknown quality option: %s\n' "$quality_option" >&2
  exit 2
fi
if [[ ! -x "$tetgen_bin" ]]; then
  printf 'TetGen executable is not runnable: %s\n' "$tetgen_bin" >&2
  exit 2
fi

repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-shell.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT

c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/export_shell.cpp" \
  -o "$scratch_dir/export_shell"
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/verify_shell.cpp" \
  -o "$scratch_dir/verify_shell"

prefix="$scratch_dir/shell"
"$scratch_dir/export_shell" "$prefix.poly" "$resolution" "$phase_x" "$phase_y"
"$tetgen_bin" -pYM "$prefix.poly"
"$scratch_dir/verify_shell" "$prefix" "$resolution" ${quality_option:+"$quality_option"}
