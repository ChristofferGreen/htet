#!/usr/bin/env bash
# Reproduce the bounded finite-triangle cross-face arrangement witness.  It
# deliberately proves only a one-triangle/two-tet seam primitive; it does not
# claim that the clipped multi-triangle corpus is a completed buffer mesher.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
build_dir=${1:-"$repo_root/build/release"}
cmake --build "$build_dir" --target arrangement_aware_cleaving_probe -j 4
"$build_dir/arrangement_aware_cleaving_probe"
