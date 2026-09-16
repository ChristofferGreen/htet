#!/usr/bin/env bash
# Compare source and owned seed-cavity traces as unordered geometric tetrahedra.
# Slot numbers and each tet's local vertex order are implementation details, so
# neither may be used to locate the first semantic divergence.
set -euo pipefail

if [[ $# != 2 ]]; then
  echo "usage: $0 AUTHOR_TRACE OWNED_TRACE" >&2
  exit 2
fi

author=$1
owned=$2
work=$(mktemp -d "${TMPDIR:-/tmp}/wang-seed-compare.XXXXXX")
trap 'rm -rf "${work}"' EXIT

# A trace row is a tet as four whitespace-separated vertex strings.  Canonical
# form sorts those vertices, preserving duplicate rows for multiplicity checks.
canonicalize() {
  local trace=$1 tag=$2 output=$3
  awk -v tag="${tag}" '
    $1 == tag {
      stage=$2
      n=0
      for (i=3; i<=NF; ++i) vertices[++n]=$i
      for (i=1; i<=n; ++i)
        for (j=i+1; j<=n; ++j)
          if (vertices[j] < vertices[i]) { t=vertices[i]; vertices[i]=vertices[j]; vertices[j]=t }
      line=stage
      for (i=1; i<=n; ++i) line=line " " vertices[i]
      print line
    }
  ' "${trace}" | LC_ALL=C sort -k1,1n -k2,2 >"${output}"
}

for tag in seed_original_working seed_original_adjusted; do
  canonicalize "${author}" "${tag}" "${work}/author-${tag}"
  canonicalize "${owned}" "${tag}" "${work}/owned-${tag}"

  if ! cmp -s "${work}/author-${tag}" "${work}/owned-${tag}"; then
    echo "seed trace diverged: ${tag}" >&2
    # Inputs are numeric-stage ordered, so the first hunk is the earliest
    # semantic mismatch without repeatedly rescanning the whole trace.
    diff -u "${work}/author-${tag}" "${work}/owned-${tag}" | head -80 >&2 || true
    exit 1
  fi
done

echo "seed cavity stages match"
