#!/usr/bin/env bash
set -euo pipefail

# TetGen is supplied by the caller and remains outside CMake and the runtime.
tetgen_bin=${1:?usage: $0 /absolute/path/to/tetgen [output-directory]}
result_dir=${2:-artifacts/nonmatching-plc-offline-oracle}
[[ -x "$tetgen_bin" ]] || { printf 'TetGen executable is not runnable: %s\n' "$tetgen_bin" >&2; exit 2; }
mkdir -p "$result_dir"
prefix="$result_dir/nonmatching-plc"
python3 scripts/nonmatching_plc_offline_oracle.py export "$prefix"
# -p: PLC; -Y: preserve exported outer and refined core boundaries exactly.
( cd "$result_dir" && "$tetgen_bin" -pY "$(basename "$prefix").poly" )
python3 scripts/nonmatching_plc_offline_oracle.py audit "$prefix"
