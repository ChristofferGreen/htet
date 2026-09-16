#!/usr/bin/env bash
# Rebuild the canonical-root witnesses and evaluate a bounded two-tet cavity
# stencil. TetGen creates only the existing reference witnesses; the repair is
# entirely local and never calls an external mesher after that point.
set -euo pipefail

if [[ $# -ne 1 || ! -x "$1" ]]; then
  printf 'usage: %s /absolute/path/to/tetgen\n' "$0" >&2
  exit 2
fi

repo_root=$(cd "$(dirname "$0")/.." && pwd)
tetgen_bin=$1
scratch_dir=$(mktemp -d /tmp/tetra-dc-bounded-multitet.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT

c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/export_shell.cpp" \
  -o "$scratch_dir/export_shell"
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/verify_shell.cpp" \
  -o "$scratch_dir/verify_shell"
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/multitet_cavity_probe.cpp" \
  -o "$scratch_dir/multitet_cavity_probe"
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/export_chunk_shell.cpp" \
  -o "$scratch_dir/export_chunk_shell"

for fixture in \
  'shell-n6 6 0.23 0.41' \
  'shell-n8 8 0.23 0.41' \
  'shell-n8-nearzero 8 0.0001 0.0001' \
  'shell-n8-phase2 8 0.5 0.0001' \
  'shell-n8-phase3 8 0.73 0.91'; do
  set -- $fixture
  "$scratch_dir/export_shell" "$scratch_dir/$1.poly" "$2" "$3" "$4"
  "$tetgen_bin" -pYM "$scratch_dir/$1.poly" >/dev/null
  "$scratch_dir/verify_shell" "$scratch_dir/$1" "$2"
  set +e
  "$scratch_dir/multitet_cavity_probe" "$scratch_dir" "$1"
  result=$?
  set -e
  if [[ $result -ne 1 ]]; then
    printf 'bounded multi-tet probe returned %d for %s; expected qualified rejection (1)\n' "$result" "$1" >&2
    exit 1
  fi
done

# Exercise the same repair against actual named ownership curtains.  The
# source chunk oracle has already qualified those two PLCs as a joined mesh;
# this check proves that the local replacement leaves every marker-4 curtain
# triangle bit-for-bit in its original boundary set.
for side in left right; do
  prefix="$scratch_dir/chunk-n8-$side"
  "$scratch_dir/export_chunk_shell" "$prefix.poly" "$side" 8 0.23 0.41 >/dev/null
  "$tetgen_bin" -pYM "$prefix.poly" >/dev/null
  set +e
  "$scratch_dir/multitet_cavity_probe" "$scratch_dir" "chunk-n8-$side"
  result=$?
  set -e
  if [[ $result -ne 1 ]]; then
    printf 'bounded multi-tet probe returned %d for N=8 %s curtain control; expected qualified rejection (1)\n' "$result" "$side" >&2
    exit 1
  fi
done
