#!/usr/bin/env bash
# Reproduce the bounded fixed-interface bridge rejection.  The program first
# qualifies the normal-offset collar, then proves that merging the mandatory
# shared nodes of one fixed regular core interface collapses noisy stepped DC
# triangles.  It has no external tetrahedralizer or hidden full-domain mesher.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-two-front-core.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT

c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/two_front_core_bridge_probe.cpp" \
  -o "$scratch_dir/two_front_core_bridge_probe"

for fixture in n6 n8 n8-nearzero n8-phase2 n8-phase3; do
  "$scratch_dir/two_front_core_bridge_probe" "$fixture"
done
