#!/usr/bin/env bash
# Classify the minimal retained S4 witnesses and run the bounded DC-placement
# control.  No TetGen installation is needed: the frozen inputs and outputs
# are retained evidence.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-quality-repair.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/quality_repair_probe.cpp" \
  -o "$scratch_dir/quality_repair_probe"
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/verify_shell.cpp" \
  -o "$scratch_dir/verify_shell"
"$scratch_dir/verify_shell" "$repo_root/artifacts/dc-viability-2026-09-09/shell-n6" 6
"$scratch_dir/verify_shell" "$repo_root/artifacts/dc-viability-2026-09-09/shell-n8" 8
"$scratch_dir/verify_shell" "$repo_root/artifacts/dc-viability-2026-09-09/shell-n8-quality" 8
"$scratch_dir/quality_repair_probe" "$repo_root/artifacts/dc-viability-2026-09-09" shell-n6
"$scratch_dir/quality_repair_probe" "$repo_root/artifacts/dc-viability-2026-09-09" shell-n8
