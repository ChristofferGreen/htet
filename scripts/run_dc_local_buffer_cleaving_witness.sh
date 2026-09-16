#!/usr/bin/env bash
# Reproduce the bounded, in-process local DC-triangle / regular-tet cleavage
# witness across the five-fixture corpus.  A passing exit status does not
# claim a complete shell: it proves the finite kernel and preserves the
# clipped-triangle cases that the next face-arrangement stitcher must handle.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
build_dir=${1:-"$repo_root/build/release"}
cmake --build "$build_dir" --target local_buffer_cleaving_witness_probe -j 4
for fixture in n6 n8 n8-nearzero n8-phase2 n8-phase3; do
  "$build_dir/local_buffer_cleaving_witness_probe" "$fixture"
done
