#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build/release}"
cmake --build "$build_dir" --target transitive_patch_star_probe -j 4
"$build_dir/transitive_patch_star_probe"
