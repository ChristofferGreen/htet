#!/usr/bin/env bash
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-n6-bounded-disk-search.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/n6_bounded_disk_patch_search_probe.cpp" \
  -o "$scratch_dir/n6_bounded_disk_patch_search_probe"
"$scratch_dir/n6_bounded_disk_patch_search_probe"
