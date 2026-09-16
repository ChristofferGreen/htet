#!/usr/bin/env bash
# Run the fixed-depth two-front collar under independently generated left and
# right DC requests.  It intentionally validates only the collar interface.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-canonical-collar.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT

c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/two_front_transition_probe.cpp" \
  -o "$scratch_dir/canonical_chunk_collar_probe"

for fixture in n6 n8 n8-nearzero n8-phase2 n8-phase3; do
  "$scratch_dir/canonical_chunk_collar_probe" --canonical-chunks "$fixture"
done
