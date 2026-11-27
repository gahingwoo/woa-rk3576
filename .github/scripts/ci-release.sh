#!/usr/bin/env bash
#
# Put files on the rolling pre-release tagged "ci", without ever leaving the
# repository without one.
#
#   ci-release.sh <owned-names-regex> <file>...
#
# Three workflows write to this release: winpe.yml (the WinPE image),
# ci.yml (the RK3576 tools bundle) and drivers.yml (the driver package
# rk3576-drivers-arm64.zip). Each passes a regular expression for
# the asset names it owns. Assets the caller does not own, and their lines
# in SHA256SUMS.txt, are kept.
#
# Order, so that a failure at any step leaves a usable release behind:
#   1. no release yet: create it with the files and SHA256SUMS.txt;
#   2. otherwise upload the new files and the merged SHA256SUMS.txt
#      (--clobber), move the tag to this commit, update title and notes,
#      and only then delete the caller's assets that the new set replaced.
#
# The old version deleted the release first and created it again. On
# 2026-11-24 the create was refused (HTTP 403, "Resource not accessible by
# integration") after the delete had succeeded, twice, and the repository
# had no ci release until it was made again by hand.
#
# Needs GH_TOKEN with contents: write, GITHUB_REPOSITORY, GITHUB_SHA, and
# GITHUB_WORKFLOW for the notes. Callers serialise on one concurrency group.
#
set -euo pipefail

owned=$1; shift
[ $# -gt 0 ] || { echo "no files given" >&2; exit 2; }
repo=$GITHUB_REPOSITORY
sha=$GITHUB_SHA
short=${sha::7}
date=$(date -u +%Y-%m-%d)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

names=()
for f in "$@"; do
    [ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
    n=$(basename "$f")
    [[ $n =~ $owned ]] || { echo "$n does not match $owned" >&2; exit 1; }
    names+=("$n")
done

exists=0
if gh release view ci -R "$repo" >/dev/null 2>&1; then
    exists=1
    mkdir -p "$work/old"
    gh release download ci -R "$repo" -p SHA256SUMS.txt -D "$work/old" 2>/dev/null || true
fi
touch "$work/old.sums"
[ -f "$work/old/SHA256SUMS.txt" ] && cp "$work/old/SHA256SUMS.txt" "$work/old.sums"

# Lines of assets the caller does not own, then the caller's new ones.
awk -v p="$owned" '{ n = $2; sub(/^\*/, "", n); if (n !~ p) print }' "$work/old.sums" > "$work/SHA256SUMS.txt"
for f in "$@"; do
    printf '%s  %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$(basename "$f")" >> "$work/SHA256SUMS.txt"
done
echo "--- SHA256SUMS.txt"
cat "$work/SHA256SUMS.txt"

cat > "$work/notes.md" <<EOF
The latest successful builds of main: the WinPE image (its file name
carries the commit it was built from), the RK3576 tools bundle
rk3576-tools-arm64.zip (its RK3576\\VERSION.txt names its commit) and the
RK3576 Windows driver package rk3576-drivers-arm64.zip (its
rk3576-drivers\\manifest.json names the builds its drivers come from).
Last updated from ${sha} on ${date} by the ${GITHUB_WORKFLOW:-CI} workflow.

Untested: nothing here has been run on a board. Every successful build of
main replaces what it built.
EOF
title="CI build ${short} ${date}"

if [ "$exists" = 0 ]; then
    gh release create ci "$@" "$work/SHA256SUMS.txt" -R "$repo" --target "$sha" \
        --prerelease --latest=false --title "$title" --notes-file "$work/notes.md"
    exit 0
fi

gh release upload ci "$@" "$work/SHA256SUMS.txt" -R "$repo" --clobber

if ! gh api -X PATCH "repos/$repo/git/refs/tags/ci" -f sha="$sha" -F force=true >/dev/null; then
    echo "::warning::could not move tag ci to $sha; the assets are updated, the tag still points at the previous build"
fi

gh release edit ci -R "$repo" --title "$title" --notes-file "$work/notes.md" --prerelease --latest=false

# The caller's assets that this set replaced.
gh release view ci -R "$repo" --json assets --jq '.assets[].name' | while read -r a; do
    [[ $a =~ $owned ]] || continue
    keep=0
    for n in "${names[@]}"; do [ "$a" = "$n" ] && keep=1; done
    [ "$keep" = 1 ] || { echo "removing $a"; gh release delete-asset ci "$a" -R "$repo" --yes; }
done
