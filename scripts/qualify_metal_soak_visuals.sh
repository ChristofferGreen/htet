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
import math, pathlib, sys
ref, out = map(pathlib.Path, sys.argv[1:])
limit = 0.003
for name in ('ground','flight','atmosphere-top','orbit'):
    a=(ref/(name+'.ppm')).read_bytes(); b=(out/(name+'.ppm')).read_bytes()
    if a[:a.find(b'\n255\n')+5] != b[:b.find(b'\n255\n')+5]:
        raise SystemExit(f'{name}: incompatible PPM header')
    start=a.find(b'\n255\n')+5; a=a[start:]; b=b[start:]
    value=math.sqrt(sum((x-y)**2 for x,y in zip(a,b))/len(a))/255.0
    if value > limit: raise SystemExit(f'{name}: NRMS {value:.7f} exceeds {limit:.7f}')
    print(f'{name}: NRMS {value:.7f}')
# Prove this gate is capable of rejecting a material visual regression rather
# than merely reporting a permissive threshold.  A uniform +16 channel shift
# is deliberately much larger than the observed fresh-run Metal variation.
altered=bytes(min(255, value+16) for value in a)
negative=math.sqrt(sum((x-y)**2 for x,y in zip(a,altered))/len(a))/255.0
if negative <= limit: raise SystemExit('visual negative control did not exceed NRMS limit')
print(f'negative control: NRMS {negative:.7f} (rejected)')
print(f'metal soak visual baselines passed: {out}')
PY
