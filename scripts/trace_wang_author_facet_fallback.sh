#!/usr/bin/env bash
# Diagnostic only: stop the pinned author implementation at the first real
# recoverFacebyaddinSt call.  This script is never part of the owned build or
# production path; it makes the retained A320 escalation evidence repeatable.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
author_binary=${1:-"$root/build-wang-reference/wang_author_reference_probe"}
input=${2:-"$root/third_party/FHCCT-FHC_CT/examples/Application_A320_20_surface_for_tet.vtk"}
output=${3:-/tmp/wang-author-facet-fallback.out}

exec lldb -b "$author_binary" \
  -o 'breakpoint set --file dt.cpp --line 2308' \
  -o "run full_file $input $output" \
  -o 'frame variable targetF info best dis base center' \
  -o 'expr SurTris[targetF].form[0]' \
  -o 'expr SurTris[targetF].form[1]' \
  -o 'expr SurTris[targetF].form[2]' \
  -o quit
