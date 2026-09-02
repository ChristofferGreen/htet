#!/usr/bin/env bash

set -euo pipefail

repository_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
vulkan_binary="${repository_root}/build/release/src/tetra_viewer/tetra_world"
metal_binary="${repository_root}/build/release/src/tetra_viewer/TetWorldMetal.app/Contents/MacOS/TetWorldMetal"

if [[ ! -x "${vulkan_binary}" || ! -x "${metal_binary}" ]]; then
  echo "release Vulkan and Metal applications must be built first" >&2
  exit 2
fi

validation_directory=$(mktemp -d "${TMPDIR:-/tmp}/tetworld-metal-validation.XXXXXX")
vulkan_capture="${validation_directory}/vulkan.ppm"
vulkan_log="${validation_directory}/vulkan.json"

"${vulkan_binary}" \
  --window-size=768x480 \
  --render-resolution=native \
  --gpu-atmosphere-capture="${vulkan_capture}" >"${vulkan_log}"

"${metal_binary}" \
  --metal-validate-geometry "${vulkan_capture%.ppm}.depth.pgm"

echo "validation artifacts: ${validation_directory}"
