#!/usr/bin/env bash
#
# Verify every driver package is complete: each directory that holds a .vcxproj
# must also have C source, an INF, a README and a build.cmd. Fast, no toolchain.
#
set -uo pipefail

fail=0

while IFS= read -r vcx; do
    dir=$(dirname "$vcx")
    missing=""
    for pat in '*.c' '*.h' '*.inf' 'README.md' 'build.cmd'; do
        # shellcheck disable=SC2086
        if ! compgen -G "$dir/$pat" >/dev/null; then
            missing="$missing $pat"
        fi
    done
    if [ -n "$missing" ]; then
        echo "::error file=$vcx::$dir is missing:$missing"
        fail=1
    else
        echo "ok   $dir"
    fi
done < <(find drivers -name '*.vcxproj' | sort)

# Every driver INF must bind an ACPI HID and target ARM64.
while IFS= read -r inf; do
    if ! grep -q 'NTARM64' "$inf"; then
        echo "::error file=$inf::INF does not target NTARM64"
        fail=1
    fi
    if ! grep -qE 'ACPI\\[A-Za-z0-9]+' "$inf"; then
        echo "::error file=$inf::INF has no ACPI\\<HID> hardware id"
        fail=1
    fi
done < <(find drivers -name '*.inf' | sort)

if [ "$fail" -eq 0 ]; then
    echo "structure checks passed"
fi
exit "$fail"
