#!/usr/bin/env bash
# Rebuild the canonical-root shell corpus and apply only the bounded local
# bistellar-repair experiment.  TetGen remains a caller-supplied external CPU
# oracle; this script neither promotes it to runtime code nor invokes its
# unbounded quality-refinement mode.
set -euo pipefail

if [[ $# -ne 1 || ! -x "$1" ]]; then
  printf 'usage: %s /absolute/path/to/tetgen\n' "$0" >&2
  exit 2
fi

repo_root=$(cd "$(dirname "$0")/.." && pwd)
tetgen_bin=$1
scratch_dir=$(mktemp -d /tmp/tetra-dc-bounded-repair.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT

c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/export_shell.cpp" \
  -o "$scratch_dir/export_shell"
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/quality_repair_probe.cpp" \
  -o "$scratch_dir/quality_repair_probe"

for fixture in \
  'shell-n6 6 0.23 0.41' \
  'shell-n8 8 0.23 0.41' \
  'shell-n8-nearzero 8 0.0001 0.0001' \
  'shell-n8-phase2 8 0.5 0.0001' \
  'shell-n8-phase3 8 0.73 0.91'; do
  set -- $fixture
  "$scratch_dir/export_shell" "$scratch_dir/$1.poly" "$2" "$3" "$4"
  "$tetgen_bin" -pYM "$scratch_dir/$1.poly" >/dev/null
  "$scratch_dir/quality_repair_probe" "$scratch_dir" "$1"
done
