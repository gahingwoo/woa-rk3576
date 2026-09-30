#!/usr/bin/env bash
#
# Verify every driver package is complete. Fast, no toolchain.
#
# Two styles live under drivers/: this repository's own (C, .inf, README.md,
# build.cmd) and the imported worproject drivers (C++, .inx templated per
# architecture, built through build/RockchipDrivers.sln). Each directory with a
# .vcxproj must have source (C, C++ or assembly; a header-only static library
# counts); each one that is not a static library must also ship an INF or INX.
# A driver binds a hardware ID: ACPI\<HID> for devices the firmware publishes,
# or a bus-enumerated ID such as CSAUDIO\... for a child of another driver.
#
set -uo pipefail

fail=0

while IFS= read -r vcx; do
    dir=$(dirname "$vcx")
    missing=""
    is_lib=0
    grep -q '<ConfigurationType>StaticLibrary</ConfigurationType>' "$vcx" && is_lib=1
    has_src=0
    for pat in '*.c' '*.cpp' '*.asm' 'arm64/*.asm'; do
        compgen -G "$dir/$pat" >/dev/null && has_src=1
    done
    [ "$is_lib" -eq 1 ] && compgen -G "$dir/*.h" >/dev/null && has_src=1
    if [ "$has_src" -eq 0 ]; then
        missing="$missing source"
    fi
    if [ "$is_lib" -eq 0 ]; then
        if ! compgen -G "$dir/*.inf" >/dev/null && ! compgen -G "$dir/*.inx" >/dev/null; then
            missing="$missing INF/INX"
        fi
    fi
    if [ -n "$missing" ]; then
        echo "::error file=$vcx::$dir is missing:$missing"
        fail=1
    else
        echo "ok   $dir"
    fi
done < <(find drivers -name '*.vcxproj' | sort)

# INFs are often saved as UTF-16LE with a BOM, which GNU grep cannot read.
as_utf8() {
    if [ "$(head -c 2 "$1" | od -An -tx1 | tr -d ' ')" = "fffe" ]; then
        iconv -f UTF-16 -t UTF-8 "$1"
    else
        cat "$1"
    fi
}

# Every driver INF must target ARM64 and bind a hardware ID. An INX is a
# template that stampinf expands, so NT$ARCH$ stands for NTARM64 there.
while IFS= read -r inf; do
    text=$(as_utf8 "$inf")
    if ! grep -qE 'NTARM64|NT\$ARCH\$' <<<"$text"; then
        echo "::error file=$inf::INF does not target NTARM64"
        fail=1
    fi
    if ! grep -qE '(ACPI|CSAUDIO)\\[A-Za-z0-9&_]+' <<<"$text"; then
        echo "::error file=$inf::INF has no ACPI\\<HID> or bus hardware id"
        fail=1
    fi
done < <(find drivers -name '*.inf' -o -name '*.inx' | sort)

if [ "$fail" -eq 0 ]; then
    echo "structure checks passed"
fi
exit "$fail"
