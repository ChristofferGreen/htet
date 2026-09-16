#!/usr/bin/env bash
# Reproduce the actual exposed terraced-core interface experiment.  It keeps
# full lattice (i,j,k) addresses, then rejects only the zero-buffer direct
# collar attachment; no external tetrahedralizer or remeshing is involved.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-terraced-core.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT

c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/terraced_core_interface_probe.cpp" \
  -o "$scratch_dir/terraced_core_interface_probe"

for fixture in n6 n8 n8-nearzero n8-phase2 n8-phase3; do
  "$scratch_dir/terraced_core_interface_probe" "$fixture"
done
