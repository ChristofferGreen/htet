#!/usr/bin/env bash
# Fresh finite DC roof/curtain/bottom -> regular-core cavity oracle.
# This validates one complete finite terrain volume, unlike the lower-DC
# scaffold oracle. TetGen is caller supplied and remains offline only.
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
scratch_dir=$(mktemp -d /tmp/tetra-dc-complete-volume.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/export_complete_volume.cpp" -o "$scratch_dir/export"
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/verify_shell.cpp" -o "$scratch_dir/verify"
prefix="$scratch_dir/complete"
"$scratch_dir/export" "$prefix.poly" "$resolution" "$phase_x" "$phase_y"
"$tetgen_bin" -pYq1.1 "$prefix.poly"
"$scratch_dir/verify" "$prefix" "$resolution" --require-quality
