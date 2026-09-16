#!/usr/bin/env bash
# Rebuild canonical-root witnesses and exercise a bounded strictly-interior
# Steiner/star-cavity negative-control family.  TetGen is only the existing
# external source of a valid reference mesh; the repair itself never invokes
# it or any global optimization.
set -euo pipefail

if [[ $# -ne 1 || ! -x "$1" ]]; then
  printf 'usage: %s /absolute/path/to/tetgen\n' "$0" >&2
  exit 2
fi

repo_root=$(cd "$(dirname "$0")/.." && pwd)
tetgen_bin=$1
scratch_dir=$(mktemp -d /tmp/tetra-dc-bounded-steiner.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT

c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/export_shell.cpp" \
  -o "$scratch_dir/export_shell"
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/verify_shell.cpp" \
  -o "$scratch_dir/verify_shell"
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/steiner_cavity_probe.cpp" \
  -o "$scratch_dir/steiner_cavity_probe"

for fixture in \
  'shell-n6 6 0.23 0.41' \
  'shell-n8 8 0.23 0.41' \
  'shell-n8-nearzero 8 0.0001 0.0001' \
  'shell-n8-phase2 8 0.5 0.0001' \
  'shell-n8-phase3 8 0.73 0.91'; do
  set -- $fixture
  "$scratch_dir/export_shell" "$scratch_dir/$1.poly" "$2" "$3" "$4"
  "$tetgen_bin" -pYM "$scratch_dir/$1.poly" >/dev/null
  "$scratch_dir/verify_shell" "$scratch_dir/$1" "$2"
  set +e
  "$scratch_dir/steiner_cavity_probe" "$scratch_dir" "$1"
  result=$?
  set -e
  if [[ $result -ne 1 ]]; then
    printf 'bounded Steiner probe returned %d for %s; expected qualified rejection (1)\n' "$result" "$1" >&2
    exit 1
  fi
done
