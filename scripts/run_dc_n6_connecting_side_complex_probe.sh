#!/usr/bin/env bash
# Rebuild and run the smallest explicit N6 connecting-side rejection.
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-n6-side-complex.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/n6_connecting_side_complex_probe.cpp" \
  -o "$scratch_dir/n6_connecting_side_complex_probe"
"$scratch_dir/n6_connecting_side_complex_probe"
