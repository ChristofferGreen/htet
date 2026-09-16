#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build/release}"
cmake --build "$build_dir" --target owner_neighbor_star_template -j 4
"$build_dir/owner_neighbor_star_template"
