#!/usr/bin/env bash
# Rebuild and run the bounded N6 interface-aware shared-buffer rejection.
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-terraced-shared-buffer.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/terraced_core_shared_buffer_probe.cpp" \
  -o "$scratch_dir/terraced_core_shared_buffer_probe"
"$scratch_dir/terraced_core_shared_buffer_probe"
