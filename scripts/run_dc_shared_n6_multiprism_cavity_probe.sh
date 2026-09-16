#!/usr/bin/env bash
# Rebuild and run the bounded, in-process shared N6 cavity rejection.  This
# has no TetGen dependency: it is specifically the small local construction
# that must be superseded by a terraced-core-conforming buffer.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
scratch_dir=$(mktemp -d /tmp/tetra-dc-shared-n6-cavity.XXXXXX)
trap 'rm -rf "$scratch_dir"' EXIT

c++ -O2 -std=c++23 -I "$repo_root/src" \
  "$repo_root/artifacts/dc-viability-2026-09-09/shared_n6_multiprism_cavity_probe.cpp" \
  -o "$scratch_dir/shared_n6_multiprism_cavity_probe"
"$scratch_dir/shared_n6_multiprism_cavity_probe"
