#!/usr/bin/env bash
# Reproduce the bounded structural two-front collar experiment.  This uses no
# external tetrahedralizer and deliberately does not claim a regular-grid core
# connection; it tests the actual frozen DC sheet and a finite normal-offset
# front only.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-two-front.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT

c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/two_front_transition_probe.cpp" \
  -o "$scratch_dir/two_front_transition_probe"

for fixture in n6 n8 n8-nearzero n8-phase2 n8-phase3; do
  "$scratch_dir/two_front_transition_probe" "$fixture"
done
