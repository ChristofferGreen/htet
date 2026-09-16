#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
scratch_dir="$(mktemp -d)"
trap 'rm -rf "$scratch_dir"' EXIT
c++ -std=c++23 -I"$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/n6_open_rebuildable_inner_front_probe.cpp" \
  -o "$scratch_dir/n6_open_rebuildable_inner_front_probe"
"$scratch_dir/n6_open_rebuildable_inner_front_probe"
