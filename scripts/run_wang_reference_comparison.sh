#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd "${script_dir}/.." && pwd)
author_dir="${repo_dir}/third_party/FHCCT-WYFDT_TEST"
author_build="${repo_dir}/build-wang-reference"
prototype_build="${repo_dir}/build-release"
expected_author_revision=46e41e2439979a3e7db65fb135c0fcae3d53952e
expected_companion_revision=6d0bec37347f21d59c107b3758e3fc6a90ebbacf

actual_author_revision=$(git -C "${author_dir}" rev-parse HEAD)
actual_companion_revision=$(git -C "${repo_dir}/third_party/FHCCT-FHC_CT" rev-parse HEAD)
if [[ "${actual_author_revision}" != "${expected_author_revision}" ]]; then
  echo "wrong FHCCT-WYFDT_TEST revision: ${actual_author_revision}" >&2
  exit 1
fi
if [[ "${actual_companion_revision}" != "${expected_companion_revision}" ]]; then
  echo "wrong FHCCT-FHC_CT revision: ${actual_companion_revision}" >&2
  exit 1
fi
if [[ -n $(git -C "${author_dir}" status --short) ]]; then
  echo "FHCCT-WYFDT_TEST must remain unmodified" >&2
  exit 1
fi
if [[ -n $(git -C "${repo_dir}/third_party/FHCCT-FHC_CT" status --short) ]]; then
  echo "FHCCT-FHC_CT must remain unmodified" >&2
  exit 1
fi

if command -v g++-14 >/dev/null 2>&1; then
  reference_compiler=$(command -v g++-14)
elif [[ -x /opt/homebrew/bin/g++-14 ]]; then
  reference_compiler=/opt/homebrew/bin/g++-14
else
  echo "GCC 14 is required for the pinned reference comparison" >&2
  exit 1
fi

# The pinned predicate library is a Shewchuk-style expansion implementation.
# Its Release build at this revision changes exact-zero insphere cases under
# optimisation, whereas Debug follows the source's symbolic-predicate path.
# This affects only the differential oracle, never the production target.
cmake -S "${repo_dir}/tools/wang_reference_harness" -B "${author_build}" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER="${reference_compiler}" >/dev/null
cmake --build "${author_build}" --target wang_author_reference_probe -j4 >/dev/null
cmake -S "${repo_dir}" -B "${prototype_build}" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DTETRA_BUILD_WANG_AUTHOR_ORACLE=ON >/dev/null
cmake --build "${prototype_build}" --target wang_prototype_reference_probe -j4 >/dev/null

comparison_dir=$(mktemp -d "${TMPDIR:-/tmp}/wang-reference-comparison.XXXXXX")
trap 'rm -rf "${comparison_dir}"' EXIT

if [[ "${WANG_REFERENCE_COMPARE_SCHEDULER_ONLY:-0}" != "1" ]]; then
  for fixture in tetrahedron cube; do
    input="${repo_dir}/tests/fixtures/wang/reference_${fixture%hedron}_surface.vtk"
    if [[ "${fixture}" == "tetrahedron" ]]; then
      input="${repo_dir}/tests/fixtures/wang/reference_tetra_surface.vtk"
    fi
    author_summary="${comparison_dir}/${fixture}.author.txt"
    prototype_summary="${comparison_dir}/${fixture}.prototype.txt"
    "${author_build}/wang_author_reference_probe" \
      "${fixture}" "${input}" "${author_summary}" \
      >"${comparison_dir}/${fixture}.author.log" 2>&1
    "${prototype_build}/wang_prototype_reference_probe" \
      "${fixture}" "${prototype_summary}"
    if [[ "${WANG_REFERENCE_INJECT_MISMATCH:-0}" == "1" &&
          "${fixture}" == "tetrahedron" ]]; then
      printf '%s\n' 'deliberate_mismatch 1' >>"${prototype_summary}"
    fi
    if ! diff -u "${author_summary}" "${prototype_summary}"; then
      echo "Wang reference comparison failed for ${fixture}" >&2
      exit 1
    fi
  done
fi

if [[ "${WANG_REFERENCE_COMPARE_SCHEDULER:-0}" == "1" ]]; then
  author_summary="${comparison_dir}/scheduler.author.txt"
  prototype_summary="${comparison_dir}/scheduler.prototype.txt"
  "${author_build}/wang_author_reference_probe" scheduler \
    "${repo_dir}/tests/fixtures/wang/reference_scheduler_surface.vtk" \
    "${author_summary}" >"${comparison_dir}/scheduler.author.log" 2>&1
  "${prototype_build}/wang_prototype_reference_probe" scheduler \
    "${prototype_summary}"
  # R3 is three deliberately separate source-conformance gates.  They are
  # kept separate because the author reuses node slots after the seed, while
  # the prototype exports immutable identities.  The unported seed-plan and
  # direction diagnostics are therefore not silently treated as equivalent.
  # Each selected record is byte-for-byte author output.
  gates="${WANG_REFERENCE_COMPARE_GATES:-seed state direction edge_contact cascade_synthetic first_flip local scheduler_local_prefix scheduler_full_search scheduler_pre_steiner scheduler_first_locked scheduler_steiner1 scheduler}"
  for gate in ${gates}; do
    case "${gate}" in
      seed) pattern='^(insertion_order|seed_stage_count|seed_stage |seed_stage_cell |seed_count|seed )' ;;
      # P2T is mutable source control state: finddirection starts from it.
      # Do not let topology-only equality mask a different subsequent walk.
      state) pattern='^(state_cell|state_neighbour|state_p2t)' ;;
      direction) pattern='^direction_semantic' ;;
      edge_contact) pattern='^edge_contact_synthetic' ;;
      cascade_synthetic) pattern='^cascade_synthetic' ;;
      first_flip) pattern='^(first_removeface|first_flip32|first_flip23)' ;;
      local) pattern='^local_' ;;
      # The local scheduler has to preserve its raw P2T carriers as well as
      # queue order: the next finddirection call starts from that carrier.
      scheduler_local_prefix) pattern='^(scheduler_local_prefix|scheduler_raw_local_prefix|scheduler_p2t_attempt_)' ;;
      scheduler_full_search) pattern='^scheduler_full_search' ;;
      scheduler_pre_steiner) pattern='^scheduler_pre_steiner' ;;
      scheduler_first_locked) pattern='^scheduler_steiner1_first_insert' ;;
      scheduler_steiner1) pattern='^(scheduler_steiner1_result|scheduler_steiner1_point|scheduler_steiner1_count|scheduler_steiner1 |scheduler_steiner1_.*_placement)' ;;
      scheduler) pattern='^(scheduler_event_count|scheduler )' ;;
    esac
    author_gate="${comparison_dir}/scheduler.${gate}.author.txt"
    prototype_gate="${comparison_dir}/scheduler.${gate}.prototype.txt"
    rg --no-line-number "${pattern}" "${author_summary}" >"${author_gate}"
    rg --no-line-number "${pattern}" "${prototype_summary}" >"${prototype_gate}"
    # The two probes name the producer explicitly. Compare the diagnostic
    # payload, not that label, so source fixedSplitPoint inputs are retained
    # as a strict alignment gate.
    if [[ "${gate}" == "scheduler_steiner1" ]]; then
      sed -i.bak -E 's/scheduler_steiner1_(reference|owned)_placement/scheduler_steiner1_placement/' \
        "${author_gate}" "${prototype_gate}"
      rm -f "${author_gate}.bak" "${prototype_gate}.bak"
    fi
    if ! diff -u "${author_gate}" "${prototype_gate}"; then
      echo "Wang R3 ${gate} comparison failed" >&2
      exit 1
    fi
  done
fi

echo "Wang R2 reference comparison passed"
echo "author revision: ${actual_author_revision}"
echo "companion revision: ${actual_companion_revision}"
echo "reference compiler: $(${reference_compiler} --version | head -n 1)"
echo "configuration: reference-build=Debug constrain=1 ignoreIntersect=0 autoflip=1 refine=0 optlevel=0 nthread=1 infolevel=2"
