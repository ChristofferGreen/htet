#!/usr/bin/env bash
# External feasibility oracle for the 20 actual face-connected N6 bad-shell
# cavities.  TetGen is deliberately caller-supplied and never a fallback.
set -euo pipefail
if [[ $# -ne 1 || ! -x "$1" ]]; then
  printf 'usage: %s /absolute/path/to/tetgen\n' "$0" >&2
  exit 2
fi
repo_root=$(cd "$(dirname "$0")/.." && pwd)
tetgen_bin=$1
scratch_dir=$(mktemp -d /tmp/n6-local-plc-quality-oracle.XXXXXX)
oracle="$scratch_dir/oracle"
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/n6_local_plc_quality_oracle.cpp" -o "$oracle"

for option in q1.1 q1.2 q1.3 q1.4 q1.6 q2.0; do
  result_dir="$scratch_dir/$option"
  "$oracle" export "$result_dir" >/dev/null
  for plc in "$result_dir"/region-*.poly; do "$tetgen_bin" -pY"$option" "$plc" >/dev/null; done
  printf '%s ' "$option"
  "$oracle" verify "$result_dir"
  ( cd "$result_dir" && shasum -a 256 manifest.txt region-*.poly region-*.map region-*.1.node region-*.1.ele ) >"$result_dir/SHA256SUMS"
done
printf 'retained oracle outputs: %s\n' "$scratch_dir"
