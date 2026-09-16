#!/usr/bin/env bash
# Reproduce the bounded uncut-grid-scaffold rejection.  This intentionally
# uses no external tetrahedralizer: it proves that unchanged regular faces do
# not conform to the qualified free collar front, so no buffer tets are emitted.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-scaffolded-buffer.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT

c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/scaffolded_interfront_buffer_probe.cpp" \
  -o "$scratch_dir/scaffolded_interfront_buffer_probe"

for fixture in n6 n8 n8-nearzero n8-phase2 n8-phase3; do
  "$scratch_dir/scaffolded_interfront_buffer_probe" "$fixture"
done
