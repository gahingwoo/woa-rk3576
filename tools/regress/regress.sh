#!/usr/bin/env bash
# regress.sh - the RK3576 Linux regression suite (Armbian, Fedora; device
# tree boot), the counterpart of regress.ps1: one PASS / FAIL / SKIP / N-A
# row per check, for the same three boards and the same board manifests.
#
#   sudo ./regress.sh [--board auto|tspi3m|cm5io|rock4d] [--quick] [--out DIR]
#                     [--require nvme,sd,...]
#
# Reads only. It loads no module, binds or unbinds nothing, changes no
# setting and never powers off or reboots (poweroff is listed, not tested).
# What it writes: the results folder and one temporary file for the storage
# check, deleted afterwards. Root is needed for dmesg on most distributions;
# without it that check is a SKIP.
#
# Output: DIR/results.json and DIR/results.md (default
# /var/tmp/rk3576-regress/<board>-<time>). Exit code: 0 no FAIL, 1 at least
# one FAIL, 2 the suite could not run.
#
# Tests: REGRESS_ROOT=<dir> reads /sys and /proc under that folder, and
# commands (dmesg, ip, ping, timedatectl) are looked up in PATH, so
# tests/test-regress-sh.sh runs the suite against a made-up board.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

set -u
LC_ALL=C
export LC_ALL

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${REGRESS_ROOT:-}
BOARD=auto
QUICK=0
OUT=
REQUIRE=,

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case "$1" in
        --board) BOARD=$2; shift 2 ;;
        --quick) QUICK=1; shift ;;
        --out) OUT=$2; shift 2 ;;
        --require) REQUIRE=",$2,"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

START_ISO=$(date +%Y-%m-%dT%H:%M:%S)
RESULTS=$(mktemp "${TMPDIR:-/tmp}/rk3576-regress.XXXXXX")
trap 'rm -f "$RESULTS"' EXIT

# ------------------------------------------------------------------ helpers

# one result row: id, name, status, measured, expected, reason (tab-separated)
add() {
    local id=$1 name=$2 st=$3 val=${4:-} exp=${5:-} why=${6:-}
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$id" "$name" "$st" "${val//$'\t'/ }" "${exp//$'\t'/ }" "${why//$'\t'/ }" >> "$RESULTS"
    local line
    line=$(printf '%-4s %-22s %s' "$st" "$id" "$val")
    [ -n "$why" ] && line="$line  ($why)"
    echo "$line"
}

rd() { local f="$ROOT$1"; [ -r "$f" ] && tr -d '\0' < "$f" 2>/dev/null; }

# the manifest's linux.<key> (or a top-level key with a dot path)
manifest_get() {
    local file=$1 key=$2
    if command -v python3 >/dev/null 2>&1; then
        python3 -I -c '
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
for k in sys.argv[2].split("."):
    d = d.get(k) if isinstance(d, dict) else None
if d is None: print("")
elif isinstance(d, bool): print("true" if d else "false")
elif isinstance(d, list): print("\n".join(str(x) for x in d))
else: print(d)
' "$file" "$key"
    elif command -v jq >/dev/null 2>&1; then
        jq -r ".$key | if . == null then \"\" elif type == \"array\" then .[] else . end" "$file"
    else
        echo "regress.sh needs python3 or jq to read the board manifest" >&2
        exit 2
    fi
}

M() { manifest_get "$MANIFEST" "$1"; }

# yes | no | optional; --require turns optional into yes
cap() {
    local v=$1 name=${2:-}
    if [ -n "$name" ] && [[ "$REQUIRE" == *",$name,"* ]]; then echo yes; return; fi
    case "$v" in
        true|yes) echo yes ;;
        optional) echo optional ;;
        *) echo no ;;
    esac
}

# a missing thing: FAIL when required, SKIP when optional
missing() {
    local c=$1 id=$2 name=$3 exp=$4 why=$5
    if [ "$c" = optional ]; then add "$id" "$name" SKIP "" "$exp" "optional here: $why"
    else add "$id" "$name" FAIL "" "$exp" "$why"; fi
}

json_str() {
    local s=$1
    s=${s//\\/\\\\}; s=${s//\"/\\\"}; s=${s//$'\n'/ }; s=${s//$'\r'/ }
    printf '"%s"' "$s"
}

# ------------------------------------------------------------------ setup

compat=
for f in "$ROOT/proc/device-tree/compatible" "$ROOT/sys/firmware/devicetree/base/compatible"; do
    if [ -r "$f" ]; then compat=$(tr '\0' ' ' < "$f"); compat=${compat% }; break; fi
done
dmi_product=$(rd /sys/class/dmi/id/product_name)
dmi_bios=$(rd /sys/class/dmi/id/bios_version)

if [ "$BOARD" = auto ]; then
    BOARD=
    for f in "$HERE"/boards/*.json; do
        id=$(manifest_get "$f" id)
        dtc=$(manifest_get "$f" dtCompatible)
        prod=$(manifest_get "$f" smbios.product)
        if { [ -n "$dtc" ] && [[ " $compat " == *"$dtc"* ]]; } || { [ -n "$dmi_product" ] && [ "$dmi_product" = "$prod" ]; }; then
            BOARD=$id; break
        fi
    done
    if [ -z "$BOARD" ]; then
        echo "Unknown board: compatible '$compat', DMI '$dmi_product'. Pass --board." >&2
        exit 2
    fi
    detected=detected
else
    detected="command line"
fi
MANIFEST="$HERE/boards/$BOARD.json"
[ -r "$MANIFEST" ] || { echo "no manifest $MANIFEST" >&2; exit 2; }
NAME=$(M name)

[ -z "$OUT" ] && OUT="/var/tmp/rk3576-regress/$BOARD-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT" || exit 2
OUT=$(cd "$OUT" && pwd)
SUITE=unknown
[ -r "$HERE/VERSION" ] && SUITE=$(head -n1 "$HERE/VERSION")
KERNEL=$(uname -r)
OS=$( (. "$ROOT/etc/os-release" 2>/dev/null && echo "$PRETTY_NAME") || echo unknown)
[ -z "$OS" ] && OS=unknown

echo "RK3576 regression suite (Linux) $SUITE: $NAME ($BOARD, $detected)"
echo "kernel $KERNEL, $OS"
echo "results: $OUT"
echo

# ------------------------------------------------------------------ checks

# board and firmware
dtc=$(M dtCompatible)
if [[ " $compat " == *"$dtc"* ]]; then
    add board.dt "Board identity (device tree compatible)" PASS "$compat" "$dtc"
elif [ -n "$compat" ]; then
    add board.dt "Board identity (device tree compatible)" FAIL "$compat" "$dtc" "another board's device tree"
else
    add board.dt "Board identity (device tree compatible)" SKIP "" "$dtc" "no device tree (ACPI boot?)"
fi
plat=$(M smbios.platformName)
if [ -z "$dmi_bios" ]; then
    add fw.banner "Firmware version string (SMBIOS)" SKIP "" "rk3576-$plat-*" "no SMBIOS: booted through U-Boot"
elif [[ "$dmi_bios" == rk3576-"$plat"-* ]]; then
    add fw.banner "Firmware version string (SMBIOS)" PASS "$dmi_bios" "rk3576-$plat-*"
else
    add fw.banner "Firmware version string (SMBIOS)" FAIL "$dmi_bios" "rk3576-$plat-*" "not a build of this board"
fi

# CPUs
online=$(rd /sys/devices/system/cpu/online)
n=0
IFS=, read -r -a parts <<< "$online"
for p in "${parts[@]}"; do
    if [[ "$p" == *-* ]]; then n=$((n + ${p#*-} - ${p%-*} + 1)); elif [ -n "$p" ]; then n=$((n + 1)); fi
done
want=$(M cpu.count)
if [ "$n" = "$want" ]; then add cpu.count "CPUs online" PASS "$n ($online)" "$want"
else add cpu.count "CPUs online" FAIL "$n ($online)" "$want"; fi
maxk=0; freqs=
for f in "$ROOT"/sys/devices/system/cpu/cpufreq/policy*/cpuinfo_max_freq; do
    [ -r "$f" ] || continue
    k=$(cat "$f"); freqs="$freqs $((k / 1000))"
    [ "$k" -gt "$maxk" ] && maxk=$k
done
minmhz=$(M cpu.minMHz)
if [ "$maxk" -eq 0 ]; then
    add cpu.freq "CPU maximum frequency (cpufreq)" FAIL "" ">= $minmhz MHz" "no cpufreq policy"
elif [ $((maxk / 1000)) -ge "$minmhz" ]; then
    add cpu.freq "CPU maximum frequency (cpufreq)" PASS "$((maxk / 1000)) MHz (policies:$freqs)" ">= $minmhz MHz"
else
    add cpu.freq "CPU maximum frequency (cpufreq)" FAIL "$((maxk / 1000)) MHz (policies:$freqs)" ">= $minmhz MHz"
fi

# kernel log
if out=$(dmesg --level=err,crit,alert,emerg 2>/dev/null); then
    ign=$(M linux.dmesgIgnore | paste -sd'|' -)
    if [ -n "$ign" ]; then out=$(printf '%s\n' "$out" | grep -Ev -- "$ign"); fi
    out=$(printf '%s\n' "$out" | sed '/^[[:space:]]*$/d')
    cnt=$(printf '%s' "$out" | grep -c . || true)
    printf '%s\n' "$out" > "$OUT/dmesg-errors.txt"
    if [ "$cnt" -eq 0 ]; then add dmesg "Kernel log: no err/crit/alert/emerg lines" PASS 0 "0 outside linux.dmesgIgnore"
    else add dmesg "Kernel log: no err/crit/alert/emerg lines" FAIL "$cnt: $(printf '%s' "$out" | head -n 3 | tr '\n' ';')" "0 outside linux.dmesgIgnore" "all in dmesg-errors.txt"; fi
else
    add dmesg "Kernel log: no err/crit/alert/emerg lines" SKIP "" 0 "dmesg not readable (run as root)"
fi

# Ethernet
c=$(cap "$(M linux.ethernet)")
if [ "$c" = no ]; then add net.ethernet "Ethernet bound, up, IPv4" N-A "" "" "no Ethernet"
else
    eth=
    for d in "$ROOT"/sys/class/net/*; do
        [ -e "$d/device/driver" ] || continue
        drv=$(basename "$(readlink "$d/device/driver")")
        case "$drv" in *dwmac*|*stmmac*|*gmac*) eth=$(basename "$d"); ethdrv=$drv; break ;; esac
    done
    if [ -z "$eth" ]; then missing "$c" net.ethernet "Ethernet bound, up, IPv4" "rk_gmac-dwmac, up" "no interface bound to the GMAC driver"
    else
        state=$(rd "/sys/class/net/$eth/operstate")
        ip4=$(ip -4 -o addr show dev "$eth" 2>/dev/null | awk '{print $4}' | head -n1)
        if [ "$state" = up ] && [ -n "$ip4" ]; then
            add net.ethernet "Ethernet bound, up, IPv4" PASS "$eth ($ethdrv) $state $ip4" "up, IPv4"
            gw=$(ip -4 route show default dev "$eth" 2>/dev/null | awk '/default/ {print $3; exit}')
            if [ -z "$gw" ]; then add net.gateway "Ping the gateway" SKIP "" "replies" "no default route on $eth"
            elif r=$(ping -c 3 -W 2 "$gw" 2>/dev/null) || [ -n "$r" ]; then
                got=$(printf '%s\n' "$r" | sed -n 's/.* \([0-9]*\) received.*/\1/p')
                if [ "${got:-0}" -gt 0 ]; then add net.gateway "Ping the gateway" PASS "$gw: $got/3" ">= 1 of 3"
                else add net.gateway "Ping the gateway" FAIL "$gw: 0/3" ">= 1 of 3"; fi
            else add net.gateway "Ping the gateway" FAIL "$gw: no reply" ">= 1 of 3"; fi
        else
            add net.ethernet "Ethernet bound, up, IPv4" FAIL "$eth ($ethdrv) $state ${ip4:-no IPv4}" "up, IPv4" "cable or DHCP?"
        fi
    fi
fi

# USB
want=$(M linux.usbControllers)
ctrl=$(for u in "$ROOT"/sys/bus/usb/devices/usb*; do [ -e "$u" ] && dirname "$(readlink -f "$u")"; done | sort -u | grep -c . || true)
if [ "$ctrl" -ge "$want" ]; then add usb.controllers "USB host controllers" PASS "$ctrl" ">= $want"
else add usb.controllers "USB host controllers" FAIL "$ctrl" ">= $want"; fi
devs=0; hubs=0; list=
for u in "$ROOT"/sys/bus/usb/devices/[0-9]*-*; do
    [ -e "$u" ] || continue
    case "$(basename "$u")" in *:*) continue ;; esac
    devs=$((devs + 1))
    cls=$(cat "$u/bDeviceClass" 2>/dev/null)
    [ "$cls" = 09 ] && hubs=$((hubs + 1))
    list="$list $(cat "$u/idVendor" 2>/dev/null):$(cat "$u/idProduct" 2>/dev/null)"
done
c=$(cap "$(M linux.usbHub)")
if [ "$c" = yes ] && [ "$hubs" -lt 1 ]; then add usb.devices "USB devices enumerated" FAIL "$devs device(s):$list" "the onboard hub" "no hub enumerated"
else add usb.devices "USB devices enumerated" PASS "$devs device(s):$list" "enumerates"; fi

# Storage
for k in emmc sd nvme; do
    c=$(cap "$(M "linux.$k")" "$k")
    case $k in emmc) label=eMMC ;; sd) label="SD card" ;; nvme) label=NVMe ;; esac
    if [ "$c" = no ]; then add "storage.$k" "Storage: $label" N-A "" "" "no $label on this board"; continue; fi
    found=
    if [ "$k" = nvme ]; then
        for b in "$ROOT"/sys/block/nvme*n*; do [ -e "$b" ] && found="$found $(basename "$b") $(( $(cat "$b/size") / 2097152 )) GiB"; done
    else
        want=MMC; [ "$k" = sd ] && want=SD
        for b in "$ROOT"/sys/block/mmcblk*; do
            [ -e "$b/device/type" ] || continue
            [ "$(cat "$b/device/type")" = "$want" ] && found="$found $(basename "$b") $(( $(cat "$b/size") / 2097152 )) GiB"
        done
    fi
    if [ -n "$found" ]; then add "storage.$k" "Storage: $label" PASS "${found# }" present
    else missing "$c" "storage.$k" "Storage: $label" present "not found"; fi
done
mib=64; [ "$QUICK" = 1 ] && mib=16
tdir=${REGRESS_TMPDIR:-/var/tmp}
tf="$tdir/rk3576-regress-$$.bin"
if dd if=/dev/urandom of="$tf" bs=1M count=$mib conv=fsync status=none 2>/dev/null; then
    a=$(sha256sum < "$tf" | cut -d' ' -f1)
    # the read back bypasses the page cache where the file system allows it
    b=$( (dd if="$tf" bs=1M iflag=direct status=none 2>/dev/null || cat "$tf") | sha256sum | cut -d' ' -f1)
    rm -f "$tf"
    if [ "$a" = "$b" ]; then add storage.rw "Storage: write, read back, compare ($tdir)" PASS "$mib MiB" "SHA-256 equal"
    else add storage.rw "Storage: write, read back, compare ($tdir)" FAIL "$mib MiB" "SHA-256 equal" "read back differs"; fi
else
    rm -f "$tf"
    add storage.rw "Storage: write, read back, compare ($tdir)" FAIL "" "SHA-256 equal" "could not write $tf"
fi

# HDMI
c=$(cap "$(M linux.hdmi)")
if [ "$c" = no ]; then add display.hdmi "HDMI connector connected" N-A "" "" "no HDMI"
else
    st=; con=
    for d in "$ROOT"/sys/class/drm/card*-HDMI-A-*; do
        [ -e "$d/status" ] || continue
        con=$(basename "$d"); st=$(cat "$d/status"); [ "$st" = connected ] && break
    done
    if [ "$st" = connected ]; then
        mode=$(head -n1 "$ROOT/sys/class/drm/$con/modes" 2>/dev/null)
        add display.hdmi "HDMI connector connected" PASS "$con connected${mode:+, first mode $mode}" connected
    elif [ -n "$con" ]; then add display.hdmi "HDMI connector connected" FAIL "$con $st" connected "monitor plugged in?"
    else missing "$c" display.hdmi "HDMI connector connected" connected "no HDMI connector in DRM"; fi
fi

# GPU
c=$(cap "$(M linux.gpu)")
if [ "$c" = no ]; then add gpu.driver "GPU bound to panfrost" N-A "" "" ""
else
    g=
    for x in "$ROOT"/sys/bus/platform/drivers/panfrost/*.gpu; do [ -e "$x" ] && { g=$(basename "$x"); break; }; done
    if [ -n "$g" ]; then add gpu.driver "GPU bound to panfrost" PASS "$g" panfrost
    else missing "$c" gpu.driver "GPU bound to panfrost" panfrost "no device bound to panfrost"; fi
fi

# NPU and video decoder: only reported when the kernel has drivers for them
c=$(cap "$(M linux.npu)")
if [ "$c" != no ]; then
    if ls "$ROOT"/sys/class/accel/accel* >/dev/null 2>&1 || ls "$ROOT"/sys/bus/platform/drivers/rocket/*.npu >/dev/null 2>&1; then
        add npu.driver "NPU driver bound" PASS "$(ls "$ROOT"/sys/class/accel 2>/dev/null | tr '\n' ' ')" "an accel device"
    else missing "$c" npu.driver "NPU driver bound" "an accel device" "no accel device (kernel without rocket?)"; fi
fi
c=$(cap "$(M linux.vdec)")
if [ "$c" != no ]; then
    v=
    for d in "$ROOT"/sys/class/video4linux/video*; do
        [ -r "$d/name" ] && grep -qi rkvdec "$d/name" && { v=$(basename "$d"); break; }
    done
    if [ -n "$v" ]; then add vdec.driver "Video decoder (rkvdec) bound" PASS "$v" rkvdec
    else missing "$c" vdec.driver "Video decoder (rkvdec) bound" rkvdec "no rkvdec video device"; fi
fi

# Thermal
want=$(M linux.thermalZones)
z=0; bad=; vals=
for d in "$ROOT"/sys/class/thermal/thermal_zone*; do
    [ -r "$d/temp" ] || continue
    t=$(cat "$d/temp" 2>/dev/null) || { bad="$bad $(basename "$d")"; continue; }
    z=$((z + 1)); tc=$((t / 1000))
    vals="$vals $(cat "$d/type" 2>/dev/null)=${tc}C"
    if [ "$tc" -le 0 ] || [ "$tc" -ge "$(M thermal.maxC)" ]; then bad="$bad $(cat "$d/type")"; fi
done
if [ "$z" -ge "$want" ] && [ -z "$bad" ]; then add thermal.zones "Thermal zones readable" PASS "$z:$vals" ">= $want zones, 0..$(M thermal.maxC) C"
else add thermal.zones "Thermal zones readable" FAIL "$z:$vals" ">= $want zones, 0..$(M thermal.maxC) C" "${bad:+out of range or unreadable:$bad}"; fi

# Fan
c=$(cap "$(M linux.fan)")
if [ "$c" = no ]; then add fan.pwm "PWM fan" N-A "" "" "no fan"
else
    fan=
    for h in "$ROOT"/sys/class/hwmon/hwmon*; do
        [ "$(cat "$h/name" 2>/dev/null)" = pwmfan ] && { fan=$(basename "$h"); pwm=$(cat "$h/pwm1" 2>/dev/null); break; }
    done
    if [ -n "$fan" ]; then add fan.pwm "PWM fan" PASS "$fan pwm1=${pwm:-?}" "a pwmfan hwmon"
    else missing "$c" fan.pwm "PWM fan" "a pwmfan hwmon" "no pwm-fan in this device tree"; fi
fi

# RTC and time
rtcname=$(M linux.rtc)
rtc=; rtcdrv=
for r in "$ROOT"/sys/class/rtc/rtc*; do
    [ -e "$r/name" ] || continue
    if grep -qi "$rtcname" "$r/name"; then rtc=$(basename "$r"); rtcdrv=$(cat "$r/name"); break; fi
done
if [ -n "$rtc" ]; then
    rt="$(cat "$ROOT/sys/class/rtc/$rtc/date" 2>/dev/null) $(cat "$ROOT/sys/class/rtc/$rtc/time" 2>/dev/null)"
    add time.rtc "RTC ($rtcname)" PASS "$rtc $rtcdrv: $rt UTC" "$rtcname"
else
    add time.rtc "RTC ($rtcname)" FAIL "" "$rtcname" "no RTC device of that name"
fi
year=$(date +%Y)
sync=$(timedatectl show -p NTPSynchronized --value 2>/dev/null)
note=; [ "$(M rtc.battery)" = false ] && note=$(M rtc.note)
if [ "$year" -lt 2026 ] || [ "$year" -gt 2040 ]; then add time.clock "Time: clock sane" FAIL "$(date -Iseconds)" "year 2026..2040" "the year is wrong${note:+; $note}"
else add time.clock "Time: clock sane" PASS "$(date -Iseconds), NTP synchronized: ${sync:-unknown}" "year 2026..2040" "$note"; fi

add power.off "Power off" N-A "" "" "not tested: the suite never powers the board off"

# ------------------------------------------------------------------ report

pass=$(awk -F'\t' '$3=="PASS"' "$RESULTS" | wc -l)
fail=$(awk -F'\t' '$3=="FAIL"' "$RESULTS" | wc -l)
skip=$(awk -F'\t' '$3=="SKIP"' "$RESULTS" | wc -l)
na=$(awk -F'\t' '$3=="N-A"' "$RESULTS" | wc -l)
END_ISO=$(date +%Y-%m-%dT%H:%M:%S)

{
    printf '{\n  "suite": %s,\n  "os": "linux",\n  "board": %s,\n  "boardName": %s,\n' "$(json_str "$SUITE")" "$(json_str "$BOARD")" "$(json_str "$NAME")"
    printf '  "firmware": %s,\n  "kernel": %s,\n  "distribution": %s,\n' "$(json_str "$dmi_bios")" "$(json_str "$KERNEL")" "$(json_str "$OS")"
    printf '  "started": %s,\n  "finished": %s,\n  "quick": %s,\n' "$(json_str "$START_ISO")" "$(json_str "$END_ISO")" "$([ "$QUICK" = 1 ] && echo true || echo false)"
    printf '  "counts": { "PASS": %d, "FAIL": %d, "SKIP": %d, "N-A": %d },\n  "results": [\n' "$pass" "$fail" "$skip" "$na"
    first=1
    while IFS=$'\t' read -r id name st val exp why; do
        [ $first = 1 ] || printf ',\n'
        first=0
        printf '    { "id": %s, "name": %s, "status": %s, "value": %s, "expected": %s, "reason": %s }' \
            "$(json_str "$id")" "$(json_str "$name")" "$(json_str "$st")" "$(json_str "$val")" "$(json_str "$exp")" "$(json_str "$why")"
    done < "$RESULTS"
    printf '\n  ]\n}\n'
} > "$OUT/results.json"

cell() { local s=$1; s=${s//|/\\|}; printf '%s' "$s"; }
{
    echo "# RK3576 regression (Linux): $NAME"
    echo
    echo "| | |"
    echo "|---|---|"
    echo "| Board | $BOARD ($detected) |"
    echo "| Firmware | $(cell "${dmi_bios:-no SMBIOS}") |"
    echo "| Kernel | $(cell "$KERNEL"), $(cell "$OS") |"
    echo "| Suite | $SUITE$([ "$QUICK" = 1 ] && echo ', --quick') |"
    echo "| Run | $START_ISO to $END_ISO |"
    echo "| Result | PASS $pass, FAIL $fail, SKIP $skip, N-A $na |"
    echo
    echo "| id | check | result | measured | expected | reason |"
    echo "|---|---|---|---|---|---|"
    while IFS=$'\t' read -r id name st val exp why; do
        echo "| $id | $(cell "$name") | **$st** | $(cell "$val") | $(cell "$exp") | $(cell "$why") |"
    done < "$RESULTS"
} > "$OUT/results.md"

echo
echo "PASS $pass  FAIL $fail  SKIP $skip  N-A $na   -> $OUT"
[ "$fail" -gt 0 ] && exit 1
exit 0
