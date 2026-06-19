#!/usr/bin/env bash
#
# Validate relative Markdown links: every ](target) that is not an external URL
# or a pure #anchor must resolve to an existing file or directory, relative to
# the Markdown file it appears in.
#
set -uo pipefail

fail=0

while IFS= read -r md; do
    dir=$(dirname "$md")
    while IFS= read -r link; do
        case "$link" in
            http://*|https://*|mailto:*|\#*) continue ;;
        esac
        target="${link%%#*}"      # strip any #anchor
        [ -z "$target" ] && continue
        if [ ! -e "$dir/$target" ]; then
            echo "::error file=$md::broken relative link: $link"
            fail=1
        fi
    done < <(grep -oE '\]\([^)]+\)' "$md" | sed -E 's/^\]\(//; s/\)$//')
done < <(find . -path ./.git -prune -o -name '*.md' -print)

if [ "$fail" -eq 0 ]; then
    echo "markdown link checks passed"
fi
exit "$fail"
