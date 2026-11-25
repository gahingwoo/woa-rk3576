#!/usr/bin/env bash
# test-regress-sh.sh - regress.sh against made-up boards, no board needed:
# a /sys and /proc tree under a temporary folder (REGRESS_ROOT) and stub
# dmesg, ip, ping and timedatectl ahead of the real ones in PATH. Each case
# states the results it expects; any difference is a failure.
#
#   bash tests/test-regress-sh.sh
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0

# make_board DIR COMPAT [options]: a CM5-IO-like tree; options remove or break parts
make_board() {
    local r=$1 compat=$2; shift 2
    local eth_state=up hdmi=connected nvme=1 hub=1 dmi=
    for o in "$@"; do
        case "$o" in
            eth-down) eth_state=down ;;
            hdmi-off) hdmi=disconnected ;;
            no-nvme) nvme=0 ;;
            no-hub) hub=0 ;;
            dmi=*) dmi=${o#dmi=} ;;
        esac
    done
    mkdir -p "$r/proc/device-tree" "$r/sys/devices/system/cpu" "$r/etc"
    printf '%s\0rockchip,rk3576\0' "$compat" > "$r/proc/device-tree/compatible"
    echo 'PRETTY_NAME="Armbian 26.02 noble"' > "$r/etc/os-release"
    if [ -n "$dmi" ]; then
        mkdir -p "$r/sys/class/dmi/id"
        echo "${dmi%%;*}" > "$r/sys/class/dmi/id/bios_version"
        echo "${dmi#*;}" > "$r/sys/class/dmi/id/product_name"
    fi
    echo 0-7 > "$r/sys/devices/system/cpu/online"
    for p in 0 4; do
        mkdir -p "$r/sys/devices/system/cpu/cpufreq/policy$p"
        echo $(( p == 0 ? 2016000 : 2208000 )) > "$r/sys/devices/system/cpu/cpufreq/policy$p/cpuinfo_max_freq"
    done
    # GMAC
    mkdir -p "$r/sys/devices/platform/2a220000.ethernet/net/end0" "$r/sys/bus/platform/drivers/rk_gmac-dwmac" "$r/sys/class/net"
    ln -s ../../devices/platform/2a220000.ethernet/net/end0 "$r/sys/class/net/end0"
    ln -s ../../../bus/platform/drivers/rk_gmac-dwmac "$r/sys/devices/platform/2a220000.ethernet/driver"
    ln -s ../.. "$r/sys/devices/platform/2a220000.ethernet/net/end0/device"
    echo "$eth_state" > "$r/sys/devices/platform/2a220000.ethernet/net/end0/operstate"
    # two xHCI controllers, two root hubs each, a hub and a device behind it
    mkdir -p "$r/sys/bus/usb/devices"
    local n=1
    for c in 23000000.usb 23400000.usb; do
        for _ in 1 2; do
            mkdir -p "$r/sys/devices/platform/$c/xhci-hcd.$c/usb$n"
            ln -s "../../../devices/platform/$c/xhci-hcd.$c/usb$n" "$r/sys/bus/usb/devices/usb$n"
            n=$((n + 1))
        done
    done
    if [ "$hub" = 1 ]; then
        mkdir -p "$r/sys/bus/usb/devices/1-1" "$r/sys/bus/usb/devices/1-1.2" "$r/sys/bus/usb/devices/1-1:1.0"
        echo 09 > "$r/sys/bus/usb/devices/1-1/bDeviceClass"; echo 05e3 > "$r/sys/bus/usb/devices/1-1/idVendor"; echo 0610 > "$r/sys/bus/usb/devices/1-1/idProduct"
        echo 00 > "$r/sys/bus/usb/devices/1-1.2/bDeviceClass"; echo 046d > "$r/sys/bus/usb/devices/1-1.2/idVendor"; echo c52b > "$r/sys/bus/usb/devices/1-1.2/idProduct"
    fi
    # eMMC, SD, NVMe
    mkdir -p "$r/sys/block/mmcblk0/device" "$r/sys/block/mmcblk1/device"
    echo MMC > "$r/sys/block/mmcblk0/device/type"; echo 61071360 > "$r/sys/block/mmcblk0/size"
    echo SD > "$r/sys/block/mmcblk1/device/type"; echo 124735488 > "$r/sys/block/mmcblk1/size"
    if [ "$nvme" = 1 ]; then mkdir -p "$r/sys/block/nvme0n1"; echo 500118192 > "$r/sys/block/nvme0n1/size"; fi
    # HDMI
    mkdir -p "$r/sys/class/drm/card0-HDMI-A-1"
    echo "$hdmi" > "$r/sys/class/drm/card0-HDMI-A-1/status"
    printf '2560x1440\n1920x1080\n' > "$r/sys/class/drm/card0-HDMI-A-1/modes"
    # GPU
    mkdir -p "$r/sys/bus/platform/drivers/panfrost/27800000.gpu"
    # thermal: the six zones of rk3576.dtsi
    local i=0
    for z in package bigcore littlecore gpu npu ddr; do
        mkdir -p "$r/sys/class/thermal/thermal_zone$i"
        echo "$z-thermal" > "$r/sys/class/thermal/thermal_zone$i/type"
        echo $((40000 + i * 1000)) > "$r/sys/class/thermal/thermal_zone$i/temp"
        i=$((i + 1))
    done
    # RTC
    mkdir -p "$r/sys/class/rtc/rtc0"
    echo "rtc-hym8563 2-0051" > "$r/sys/class/rtc/rtc0/name"
    echo 2026-11-25 > "$r/sys/class/rtc/rtc0/date"; echo 12:00:00 > "$r/sys/class/rtc/rtc0/time"
}

make_stubs() {
    local b=$1 dmesg_text=$2
    mkdir -p "$b"
    printf '#!/bin/sh\ncat <<"EOF"\n%s\nEOF\n' "$dmesg_text" > "$b/dmesg"
    cat > "$b/ip" <<'EOF'
#!/bin/sh
case "$*" in
  *addr*) echo "2: end0    inet 192.168.1.60/24 brd 192.168.1.255 scope global end0" ;;
  *route*) echo "default via 192.168.1.1 dev end0 proto dhcp" ;;
esac
EOF
    printf '#!/bin/sh\necho "3 packets transmitted, 3 received, 0%% packet loss"\n' > "$b/ping"
    printf '#!/bin/sh\necho yes\n' > "$b/timedatectl"
    chmod +x "$b"/*
}

# run_case NAME ROOT STUBS EXIT 'id=STATUS ...' [regress.sh args]
run_case() {
    local name=$1 root=$2 stubs=$3 want_exit=$4 expect=$5; shift 5
    local out="$T/out-$name"
    echo "== $name"
    set +e
    PATH="$stubs:$PATH" REGRESS_ROOT="$root" REGRESS_TMPDIR="$T" bash "$HERE/regress.sh" --out "$out" "$@"
    local code=$?
    set -e
    if [ "$code" != "$want_exit" ]; then echo "EXIT $name: $code, expected $want_exit"; fails=$((fails + 1)); fi
    [ -s "$out/results.md" ] || { echo "MISSING $name results.md"; fails=$((fails + 1)); }
    python3 -I - "$out/results.json" "$expect" "$name" <<'EOF' || fails=$((fails + 1))
import json, sys
r = json.load(open(sys.argv[1]))
rows = {x["id"]: x for x in r["results"]}
assert len(rows) == len(r["results"]), "duplicate ids"
bad = 0
for e in sys.argv[2].split():
    k, v = e.split("=")
    got = rows.get(k, {}).get("status", "NO ROW")
    if got != v:
        print("WRONG %s: %s is %s, expected %s (%s)" % (sys.argv[3], k, got, v, rows.get(k, {}).get("reason", "")))
        bad += 1
for x in r["results"]:
    if x["status"] not in ("PASS", "FAIL", "SKIP", "N-A"):
        print("STATUS", x); bad += 1
print("checked %d expectations, %d rows" % (len(sys.argv[2].split()), len(rows)))
sys.exit(1 if bad else 0)
EOF
    if ls "$T"/rk3576-regress-*.bin >/dev/null 2>&1; then echo "LEFT a temporary file"; fails=$((fails + 1)); fi
}

# 1. CM5-IO, everything there, booted through UEFI with the DT (SMBIOS present)
make_board "$T/cm5io" armsom,cm5-io "dmi=rk3576-CM5IO-v0.4.1;CM5-IO"
make_stubs "$T/bin-clean" ""
run_case cm5io-clean "$T/cm5io" "$T/bin-clean" 0 \
  "board.dt=PASS fw.banner=PASS cpu.count=PASS cpu.freq=PASS dmesg=PASS net.ethernet=PASS net.gateway=PASS usb.controllers=PASS usb.devices=PASS storage.emmc=PASS storage.sd=PASS storage.nvme=PASS storage.rw=PASS display.hdmi=PASS gpu.driver=PASS npu.driver=SKIP vdec.driver=SKIP thermal.zones=PASS fan.pwm=SKIP time.rtc=PASS time.clock=PASS power.off=N-A"

# 2. CM5-IO with regressions: Ethernet down, no NVMe, no hub, HDMI off, a kernel error
make_board "$T/cm5io-bad" armsom,cm5-io eth-down no-nvme no-hub hdmi-off
make_stubs "$T/bin-err" "[   12.3] mmc0: error -110 whilst initialising MMC card"
run_case cm5io-bad "$T/cm5io-bad" "$T/bin-err" 1 \
  "fw.banner=SKIP dmesg=FAIL net.ethernet=FAIL usb.devices=FAIL storage.nvme=FAIL display.hdmi=FAIL"

# 3. ROCK 4D tree (no eMMC in the manifest, NVMe optional), --quick, --require sd
make_board "$T/rock4d" radxa,rock-4d no-nvme no-hub
run_case rock4d "$T/rock4d" "$T/bin-clean" 0 \
  "board.dt=PASS storage.emmc=N-A storage.sd=PASS storage.nvme=SKIP usb.devices=PASS" --quick --require sd

# 4. a TaishanPi 3M tree with another board's firmware
make_board "$T/tspi" lckfb,tspi-3m-rk3576 "dmi=rk3576-ROCK4D-v0.4.1;TaishanPi 3M"
run_case tspi-wrongfw "$T/tspi" "$T/bin-clean" 1 "board.dt=PASS fw.banner=FAIL"

# 5. an unknown board: exit 2
mkdir -p "$T/other/proc/device-tree"; printf 'pine64,quartz64\0' > "$T/other/proc/device-tree/compatible"
set +e; REGRESS_ROOT="$T/other" bash "$HERE/regress.sh" --out "$T/out-other" >/dev/null 2>&1; c=$?; set -e
if [ "$c" != 2 ]; then echo "EXIT unknown board: $c, expected 2"; fails=$((fails + 1)); else echo "== unknown board: exit 2"; fi

# 6. run-remote.sh, Linux path, with ssh and scp stubs that run "remotely"
# in a folder here: the copy, the quoting of the arguments, the exit code
# and the copy of the results back
R="$T/remote"; mkdir -p "$R" "$T/bin-ssh"
cat > "$T/bin-ssh/ssh" <<EOS
#!/usr/bin/env bash
while [ "\${1:-}" = -o ]; do shift 2; done
shift
cmd="\$*"
cmd="\${cmd//\/var\/tmp\//$R/}"
exec bash -c "\$cmd"
EOS
cat > "$T/bin-ssh/scp" <<EOS
#!/usr/bin/env bash
args=()
while [ \$# -gt 0 ]; do
  case "\$1" in -q|-r) shift ;; -o) shift 2 ;; *) a="\$1"; a="\${a#*:/var/tmp/}"; [ "\$a" != "\$1" ] && a="$R/\$a"; args+=("\$a"); shift ;; esac
done
exec cp -r "\${args[@]}"
EOS
printf '#!/bin/sh\nexit 1\n' > "$T/bin-ssh/sudo"
chmod +x "$T/bin-ssh"/*
echo "== run-remote.sh (stub ssh, Linux)"
set +e
PATH="$T/bin-ssh:$T/bin-clean:$PATH" REGRESS_ROOT="$T/rock4d" REGRESS_TMPDIR="$T" \
  bash "$HERE/run-remote.sh" --results "$T/fetched" user@board --quick --require "sd" >/dev/null 2>&1
c=$?
set -e
if [ "$c" != 0 ]; then echo "EXIT run-remote: $c, expected 0"; fails=$((fails + 1)); fi
if [ -s "$T/fetched/results.json" ] && grep -q '"quick": true' "$T/fetched/results.json" && grep -q '"board": "rock4d"' "$T/fetched/results.json"; then
  echo "run-remote: results fetched, --quick passed through"
else echo "run-remote: no results fetched"; fails=$((fails + 1)); fi
grep -q 'woa-rk3576 tools/regress' "$R/rk3576-regress-suite/VERSION" || { echo "run-remote: no VERSION"; fails=$((fails + 1)); }

echo
if [ "$fails" -gt 0 ]; then echo "test-regress-sh: $fails problem(s)"; exit 1; fi
echo "test-regress-sh: all good"
