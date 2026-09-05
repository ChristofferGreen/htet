#!/usr/bin/env bash
set -euo pipefail

# Runs the diagnostic selector against a deliberately varied camera/terrain
# corpus. It is hidden-window-only and asserts only parity/fail-closed state;
# the CPU terrain front remains the rendering authority.
repo_root=$(cd "$(dirname "$0")/.." && pwd)
binary=${1:-"$repo_root/build/release/src/tetra_viewer/tetra_world"}
if [[ ! -x "$binary" ]]; then
  echo "GPU LOD binary is not executable: $binary" >&2
  exit 2
fi

common=(--gpu-lod-diagnostic --gpu-atmosphere-benchmark --window-size=320x240 --window-position=-3000,-3000)

run_case() {
  local name=$1
  shift
  local output
  output=$("$binary" "${common[@]}" "$@")
  local line
  line=$(printf '%s\n' "$output" | rg '"event":"gpu_atmosphere_benchmark"' | tail -n 1)
  if [[ -z "$line" ]] || ! grep -q '"oracle_matches":true' <<<"$line" ||
     ! grep -q '"overflow":false' <<<"$line" ||
     ! grep -q '"failed_dispatches":0' <<<"$line"; then
    echo "GPU LOD parity failed: $name" >&2
    printf '%s\n' "$output" >&2
    exit 1
  fi
  printf '%s %s\n' "$name" "$line"
}

# The three threshold sweeps drive the same packed tuple terms as the shader.
run_case fixed --terrain-pixel-threshold=128
run_case edge-below --terrain-pixel-threshold=127.9998
run_case edge-above --terrain-pixel-threshold=128.0002
run_case field-sweep --terrain-field-pixel-threshold=16383.99
run_case limb-sweep --terrain-limb-pixel-threshold=2.000001
run_case yaw-motion --automation-yaw-sequence-degrees=1,-1,2,-2 --automation-look-frames=8
run_case walking-rebase --automation-walk-steps=96
run_case near-surface --camera-feet=0,1,0 --camera-yaw-degrees=0 --camera-pitch-degrees=-5
run_case orbital --camera-feet=0,500000,0 --camera-yaw-degrees=0 --camera-pitch-degrees=-89
run_case terrain-replacement --analytic-ridge --camera-feet=0,5,0
