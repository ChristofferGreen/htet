#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build/release}"
cmake --build "$build_dir" --target finite_patch_template_atlas -j 4
"$build_dir/finite_patch_template_atlas"
