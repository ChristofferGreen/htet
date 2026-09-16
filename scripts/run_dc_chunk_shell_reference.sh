#!/usr/bin/env bash
# Reproduce the independent-chunk shell oracle with a caller-supplied TetGen.
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
if [[ ! -x "$tetgen_bin" ]]; then printf 'TetGen executable is not runnable: %s\n' "$tetgen_bin" >&2;exit 2;fi
repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-chunk-shell.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT
c++ -O2 -std=c++23 -I "$repo_root/src" "$repo_root/artifacts/dc-viability-2026-09-09/export_chunk_shell.cpp" -o "$scratch_dir/export"
c++ -O2 -std=c++23 -I "$repo_root/src" "$repo_root/artifacts/dc-viability-2026-09-09/verify_chunk_shell.cpp" -o "$scratch_dir/verify"
build_chunk() {
  local pass=$1
  local side=$2
  local prefix="$scratch_dir/$pass-$side"
  "$scratch_dir/export" "$prefix.poly" "$side" "$resolution" "$phase_x" "$phase_y"
  "$tetgen_bin" -pYM "$prefix.poly"
}
verify_options=()
if [[ -n "$quality_option" ]]; then verify_options+=("$quality_option"); fi
build_chunk forward left
build_chunk forward right
forward=$("$scratch_dir/verify" "$scratch_dir/forward-left" "$scratch_dir/forward-right" "$resolution" "$phase_x:$phase_y" "${verify_options[@]}")
# Independent requests cannot use build order as input.  A second run reverses
# request order; the canonical-tet hashes (not a monolithic Delaunay hash)
# must agree exactly.
build_chunk reverse right
build_chunk reverse left
reverse=$("$scratch_dir/verify" "$scratch_dir/reverse-left" "$scratch_dir/reverse-right" "$resolution" "$phase_x:$phase_y" "${verify_options[@]}")
if [[ "$forward" != "$reverse" ]]; then
  printf 'independent chunks changed under reverse build order\nforward: %s\nreverse: %s\n' "$forward" "$reverse" >&2
  exit 1
fi
printf '%s\n' "$forward"
