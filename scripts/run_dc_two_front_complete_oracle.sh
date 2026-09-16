#!/usr/bin/env bash
# Reproduce the complete collar -> TetGen fill -> retained-core CPU oracle.
# It is deliberately an external existence/quality experiment, not a runtime
# mesher, chunk implementation, or GPU path.
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
[[ -x "$tetgen_bin" ]] || { printf 'TetGen executable is not runnable: %s\n' "$tetgen_bin" >&2; exit 2; }
[[ -z "$quality_option" || "$quality_option" == '--require-quality' ]] || { printf 'unknown quality option: %s\n' "$quality_option" >&2; exit 2; }
repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-two-front-complete.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT
c++ -O2 -std=c++23 -I "$repo_root/src" "$repo_root/artifacts/dc-viability-2026-09-09/export_two_front_complete.cpp" -o "$scratch_dir/export"
c++ -O2 -std=c++23 -I "$repo_root/src" "$repo_root/artifacts/dc-viability-2026-09-09/verify_two_front_complete.cpp" -o "$scratch_dir/verify"
prefix="$scratch_dir/complete"
"$scratch_dir/export" "$prefix.poly" "$resolution" "$phase_x" "$phase_y"
"$tetgen_bin" -pYM "$prefix.poly"
"$scratch_dir/verify" "$prefix" "$resolution" ${quality_option:+"$quality_option"}
