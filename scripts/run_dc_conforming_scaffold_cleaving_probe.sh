#!/usr/bin/env bash
# Reproduce the bounded contact/stencil precondition for a conforming
# scaffold-cleaving bridge.  It deliberately does not invoke an external
# tetrahedralizer or claim that the nonmatching fronts have been filled.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-conforming-cleaving.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT

c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/conforming_scaffold_cleaving_probe.cpp" \
  -o "$scratch_dir/conforming_scaffold_cleaving_probe"

for fixture in n6 n8 n8-nearzero n8-phase2 n8-phase3; do
  "$scratch_dir/conforming_scaffold_cleaving_probe" "$fixture"
done
