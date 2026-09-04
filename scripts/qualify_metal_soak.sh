#!/usr/bin/env bash
# Runs the native endurance and visual contracts from the same Release bundle.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
bin="${1:-${root}/build/release/src/tetra_viewer/TetWorldMetal.app/Contents/MacOS/TetWorldMetal}"
if [[ ! -x "$bin" ]]; then
  echo "missing executable: $bin" >&2
  exit 2
fi

out="${2:-${root}/build/metal-soak-qualification}"
mkdir -p "$out"

env TETWORLD_METAL_BACKGROUND=1 \
    TETWORLD_METAL_PREVIEW=1 \
    TETWORLD_METAL_PROFILE_INTERACTIVE=1 \
    "$bin" --metal-soak-smoke-test | tee "$out/soak.jsonl"

"${root}/scripts/qualify_metal_soak_visuals.sh" "$bin" | tee "$out/visuals.txt"
echo "metal soak qualification passed: $out"
