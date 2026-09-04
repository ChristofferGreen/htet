#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
bin="${1:-${root}/build/release/src/tetra_viewer/TetWorldMetal.app/Contents/MacOS/TetWorldMetal}"
out=$(mktemp -d "${TMPDIR:-/tmp}/tetra-metal-soak-visuals.XXXXXX")
capture() { local name=$1; shift; env TETWORLD_METAL_BACKGROUND=1 TETWORLD_METAL_PROFILE_INTERACTIVE=1 "$@" "$bin" --metal-atmosphere-capture "$out/$name.ppm" >"$out/$name.jsonl"; }
capture ground TETWORLD_METAL_REPORTED_MOUNTAIN=1
capture flight TETWORLD_METAL_ATMOSPHERE_CAPTURE_POSE=flight
capture atmosphere-top TETWORLD_METAL_ATMOSPHERE_CAPTURE_POSE=atmosphere-top
capture orbit TETWORLD_METAL_ATMOSPHERE_CAPTURE_POSE=orbit
python3 - "$root/tests/visual_baselines/metal-soak" "$out" <<'PY'
import pathlib, sys
ref, out = map(pathlib.Path, sys.argv[1:])
for name in ('ground','flight','atmosphere-top','orbit'):
    a=(ref/(name+'.ppm')).read_bytes(); b=(out/(name+'.ppm')).read_bytes()
    if a != b: raise SystemExit(f'{name}: visual baseline differs')
print(f'metal soak visual baselines passed: {out}')
PY
