#!/usr/bin/env bash

set -euo pipefail

preset=debug
run_tests=true

for argument in "$@"; do
  case "$argument" in
    --release)
      preset=release
      ;;
    --skip-tests)
      run_tests=false
      ;;
    --help)
      echo "usage: $0 [--release] [--skip-tests]"
      exit 0
      ;;
    *)
      echo "unknown argument: $argument" >&2
      echo "usage: $0 [--release] [--skip-tests]" >&2
      exit 2
      ;;
  esac
done

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repository_root"

cmake --preset "$preset"
cmake --build --preset "$preset" --parallel

if "$run_tests"; then
  ctest --test-dir "build/$preset" --output-on-failure
fi
