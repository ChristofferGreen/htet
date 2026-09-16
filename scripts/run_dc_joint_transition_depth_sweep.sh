#!/usr/bin/env bash
# Bounded negative experiment for the complete DC -> core reference volume.
#
# It changes only the *artificial* normal-offset collar front, then asks the
# same external reference mesher to close the remaining volume against the
# unchanged visible DC sheet and exact conservative Freudenthal core.  This is
# deliberately not a runtime mesher or a GPU path.  Its success condition is
# stricter and intentionally negative: every finite candidate must be a
# geometrically valid complete volume, and none may be promoted while the
# five-degree screen still fails.
set -euo pipefail

if [[ $# -ne 1 ]]; then
  printf 'usage: %s /absolute/path/to/tetgen\n' "$0" >&2
  exit 2
fi

tetgen_bin=$1
[[ -x "$tetgen_bin" ]] || { printf 'TetGen executable is not runnable: %s\n' "$tetgen_bin" >&2; exit 2; }
repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-joint-depth-sweep.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT

c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/export_two_front_complete.cpp" \
  -o "$scratch_dir/export"
c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/verify_two_front_complete.cpp" \
  -o "$scratch_dir/verify"

# A shared .90-cell front is the current qualified collar contract.  The
# deeper fronts are finite joint-construction hypotheses, not auto-selected
# runtime settings.  They remain locally valid on the whole fixture corpus.
for depth in 0.90 1.20 1.50 1.80; do
  depth_label=${depth//./_}
  for fixture in 'n6 6 .23 .41' 'n8 8 .23 .41' 'n8-nearzero 8 .0001 .0001' \
                 'n8-phase2 8 .5 .0001' 'n8-phase3 8 .73 .91'; do
    read -r name resolution phase_x phase_y <<<"$fixture"
    # TetGen derives its output stem by stripping at dots.  Keep the human
    # depth in its JSON field and use a dot-free filesystem stem.
    prefix="$scratch_dir/$name-$depth_label"
    "$scratch_dir/export" "$prefix.poly" "$resolution" "$phase_x" "$phase_y" "$depth" >/dev/null
    "$tetgen_bin" -pYMq1.4 "$prefix.poly" >/dev/null
    # First establish the full topology and both immutable interfaces.  The
    # second invocation must fail: accepting it would be a genuine quality
    # result, not a property this rejection probe can assume.
    "$scratch_dir/verify" "$prefix" "$resolution"
    if "$scratch_dir/verify" "$prefix" "$resolution" --require-quality >/dev/null; then
      printf 'unexpectedly quality-qualified candidate: fixture=%s depth=%s\n' "$name" "$depth" >&2
      exit 1
    fi
    # TetGen is still external and monolithic, but the reference witness must
    # at least be repeatable before it can falsify a construction hypothesis.
    repeat_prefix="$scratch_dir/repeat-$name-$depth_label"
    "$scratch_dir/export" "$repeat_prefix.poly" "$resolution" "$phase_x" "$phase_y" "$depth" >/dev/null
    "$tetgen_bin" -pYMq1.4 "$repeat_prefix.poly" >/dev/null
    cmp -s <(rg -v '^#' "$prefix.1.node") <(rg -v '^#' "$repeat_prefix.1.node") || {
      printf 'non-deterministic node output: fixture=%s depth=%s\n' "$name" "$depth" >&2; exit 1; }
    cmp -s <(rg -v '^#' "$prefix.1.ele") <(rg -v '^#' "$repeat_prefix.1.ele") || {
      printf 'non-deterministic tet output: fixture=%s depth=%s\n' "$name" "$depth" >&2; exit 1; }
  done
done

printf 'All 20 complete reference candidates were geometrically valid and rejected by the five-degree quality gate.\n'
