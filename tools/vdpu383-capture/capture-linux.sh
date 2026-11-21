#!/usr/bin/env bash
#
# capture-linux.sh - record what mainline Linux (rkvdec) does with the RK3576
# video decoder for one or more small H.264 clips, so the Windows bring-up
# (drivers/video/vdpu383, tools/vdpu383test) can be diffed against it.
#
# Run as root on the board under Armbian edge (kernel 7.3.x with rkvdec for
# H.264). Nothing is written to the decoder: the script decodes through the
# normal V4L2 stateless path with GStreamer and reads registers.
#
#   sudo ./capture-linux.sh                 # fetch the default clips and capture
#   sudo ./capture-linux.sh clip.264 ...    # capture these clips
#   sudo ./capture-linux.sh -n 12 clip.264  # register snapshots for 12 pictures
#
# Output: ./vdpu383-capture-<host>-<time>.tar.gz. Send that file back.
#
# What it records, per clip:
#   regs-idle.txt        decoder registers with the device awake, before decoding
#   regs-pic-NNNN.txt    the registers after decoding pictures 1..N (decode order;
#                        each snapshot is a fresh decode of the first N pictures,
#                        so it holds picture N's register file)
#   decode.i420, md5     the whole clip decoded by the hardware, cropped I420 (the
#                        format of the conformance .yuv), its MD5 and one per frame
#   ftrace-*.txt         call graph of rkvdec during a 2-picture decode, and the
#                        driver's MMIO writes if the kernel has CONFIG_TRACE_MMIO_ACCESS
# and once: kernel, clocks, power domain, CRU and PMU registers, dmesg.
#
# Register reads go through /dev/mem with 32-bit loads, only while the
# decoder is held awake (power/control = on), because a read of a powered-off
# domain is a bus error that stops the machine. If /dev/mem refuses the
# decoder's range (CONFIG_IO_STRICT_DEVMEM), the script says so and goes on
# without register snapshots; boot once with "iomem=relaxed" to get them.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: MIT
#
set -uo pipefail

NPICS=8
CLIPS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -n) NPICS="$2"; shift 2 ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) CLIPS+=("$1"); shift ;;
    esac
done

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)"; exit 1; }

HOST=$(hostname -s 2>/dev/null || echo board)
OUT="vdpu383-capture-$HOST-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
LOG="$OUT/capture.log"
exec > >(tee -a "$LOG") 2>&1

say() { printf '== %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# ------------------------------------------------------------- prerequisites

MISSING=()
have gst-launch-1.0 || MISSING+=(gstreamer1.0-tools)
have python3 || MISSING+=(python3)
have v4l2-ctl || MISSING+=(v4l-utils)
if have gst-inspect-1.0 && ! gst-inspect-1.0 v4l2slh264dec >/dev/null 2>&1; then
    MISSING+=(gstreamer1.0-plugins-bad)
fi
have gst-inspect-1.0 && ! gst-inspect-1.0 h264parse >/dev/null 2>&1 && MISSING+=(gstreamer1.0-plugins-bad)
have gst-inspect-1.0 && ! gst-inspect-1.0 videoconvert >/dev/null 2>&1 && MISSING+=(gstreamer1.0-plugins-base)
if [ ${#MISSING[@]} -gt 0 ]; then
    echo "missing: ${MISSING[*]}"
    echo "install with:  sudo apt install ${MISSING[*]}"
    exit 1
fi

# ------------------------------------------------------------------ clips

CONF=https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/AVCv1
if [ ${#CLIPS[@]} -eq 0 ]; then
    mkdir -p clips
    for c in BA1_Sony_D CABA3_SVA_B; do
        if [ ! -f "clips/$c.zip" ]; then
            say "fetching $c"
            curl -fsS -m 300 -o "clips/$c.zip" "$CONF/$c.zip" || { echo "download failed; pass clip files instead"; exit 1; }
        fi
        (cd clips && unzip -o -q "$c.zip")
    done
    CLIPS=(clips/BA1_Sony_D.jsv clips/CABA3_SVA_B.264)
fi

# ---------------------------------------------------------------- device

DEV=""
for d in /sys/bus/platform/devices/*.video-codec; do
    [ -e "$d/driver" ] || continue
    if [ "$(basename "$(readlink -f "$d/driver")")" = rkvdec ]; then
        case "$d" in *27b00000*) DEV="$d" ;; esac
    fi
done
VIDEO=""
for v in /sys/class/video4linux/video*; do
    [ "$(cat "$v/name" 2>/dev/null)" = rkvdec ] && VIDEO="/dev/$(basename "$v")"
done
say "device ${DEV:-NOT FOUND}, node ${VIDEO:-NOT FOUND}"
if [ -z "$DEV" ] || [ -z "$VIDEO" ]; then
    echo "rkvdec is not bound to 27b00000.video-codec; is this a mainline kernel with CONFIG_VIDEO_ROCKCHIP_VDEC?"
    dmesg | grep -i -E 'rkvdec|vdec' | tail -20
    exit 1
fi

# --------------------------------------------------------- register reader

cat > "$OUT/peek.py" <<'PY'
# peek.py BASE OFFSET COUNT: 32-bit loads through /dev/mem, "0xADDR 0xVALUE"
import ctypes, mmap, os, sys
base, off, count = (int(a, 0) for a in sys.argv[1:4])
page = base & ~0xFFF
size = ((base - page) + off + count * 4 + 0xFFF) & ~0xFFF
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
libc = ctypes.CDLL(None, use_errno=True)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int,
                      ctypes.c_int, ctypes.c_long]
va = libc.mmap(None, size, mmap.PROT_READ, mmap.MAP_SHARED, fd, page)
if va is None or va == ctypes.c_void_p(-1).value:
    sys.exit("mmap of 0x%x failed: %s" % (page, os.strerror(ctypes.get_errno())))
for i in range(count):
    a = base + off + i * 4
    # c_uint32.from_address(...).value is one 32-bit load
    print("0x%08x 0x%08x" % (a, ctypes.c_uint32.from_address(va + (a - page)).value))
PY
peek() { python3 "$OUT/peek.py" "$@"; }

VDEC=0x27b00000
dump_vdec() {   # $1 = file
    {
        peek $VDEC 0x000 64      # link
        peek $VDEC 0x100 384     # function registers 0..383
        peek $VDEC 0x800 16      # IOMMU 0
        peek $VDEC 0x900 16      # IOMMU 1
    } > "$1" 2>>"$OUT/peek-errors.txt"
}

hold_awake() {  # on | auto
    echo "$1" > "$DEV/power/control"
    sleep 0.2
}

# --------------------------------------------------------- environment

say "environment"
{
    uname -a
    cat /proc/cmdline
    echo
    v4l2-ctl -d "$VIDEO" --all 2>&1 | head -60
    echo
    grep -E 'STRICT_DEVMEM|TRACE_MMIO|FUNCTION_GRAPH|VIDEO_ROCKCHIP_VDEC|ROCKCHIP_IOMMU' \
        /boot/config-"$(uname -r)" 2>/dev/null || zcat /proc/config.gz 2>/dev/null | \
        grep -E 'STRICT_DEVMEM|TRACE_MMIO|FUNCTION_GRAPH|VIDEO_ROCKCHIP_VDEC|ROCKCHIP_IOMMU'
    echo
    gst-launch-1.0 --version
    gst-inspect-1.0 v4l2slh264dec | head -30
} > "$OUT/environment.txt" 2>&1
dmesg > "$OUT/dmesg-before.txt"

mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug 2>/dev/null

clocks_and_power() {    # $1 = suffix
    {
        echo "# clk_summary (rkvdec, gpll, cpll)"
        grep -E 'rkvdec|^ *gpll|^ *cpll|clk_gpll_div6' /sys/kernel/debug/clk/clk_summary 2>/dev/null
        echo "# pm_genpd_summary"
        grep -i -A3 'vdec' /sys/kernel/debug/pm_genpd/pm_genpd_summary 2>/dev/null
        echo "# runtime pm"
        cat "$DEV/power/runtime_status" "$DEV/power/control" 2>/dev/null
        echo "# CRU: PLL CPLL 0x1a0.., GPLL 0x1c0.., MODE 0x280, CLKSEL110/111, GATE45, SOFTRST45"
        peek 0x27200000 0x1a0 3
        peek 0x27200000 0x1c0 3
        peek 0x27200000 0x280 1
        peek 0x27200000 0x4b8 2
        peek 0x27200000 0x8b4 1
        peek 0x27200000 0xab4 1
        echo "# PMU: REQ0 0x110 ACK0 0x120 IDLE0 0x128 UNGATE 0x140 PWR_CON1 0x214 STATUS0 0x230 REPAIR0 0x570"
        for o in 0x110 0x120 0x128 0x140 0x214 0x230 0x570; do peek 0x27380000 $o 1; done
        echo "# QoS rkvdec 0x27f0c000"
        peek 0x27f0c000 0 8
    } > "$OUT/clocks-power-$1.txt" 2>&1
}

# -------------------------------------------------------------- per clip

decode_n() {    # $1 clip, $2 pictures (0 = all), $3 output (or empty)
    local sink="fakesink sync=false"
    [ -n "$3" ] && sink="videoconvert ! video/x-raw,format=I420 ! filesink location=$3"
    local cut=""
    [ "$2" -gt 0 ] && cut="! identity eos-after=$2"
    # shellcheck disable=SC2086
    gst-launch-1.0 -q filesrc location="$1" ! h264parse ! video/x-h264,alignment=au,stream-format=byte-stream \
        $cut ! v4l2slh264dec ! $sink
}

CAN_PEEK=1
hold_awake on
clocks_and_power awake
if ! peek $VDEC 0x100 1 > /dev/null 2>&1; then
    CAN_PEEK=0
    echo "!! /dev/mem refuses 0x27b00000 (CONFIG_IO_STRICT_DEVMEM?); no register snapshots."
    echo "!! Boot once with iomem=relaxed on the kernel command line to get them."
fi

for CLIP in "${CLIPS[@]}"; do
    name=$(basename "$CLIP"); name=${name%.*}
    D="$OUT/$name"
    mkdir -p "$D"
    cp "$CLIP" "$D/"
    say "$name: $(stat -c%s "$CLIP") bytes"

    if [ $CAN_PEEK = 1 ]; then
        dump_vdec "$D/regs-idle.txt"
        for n in $(seq 1 "$NPICS"); do
            decode_n "$CLIP" "$n" "" || echo "decode of $n pictures failed"
            dump_vdec "$D/regs-pic-$(printf %04d "$n").txt"
        done
    fi

    say "$name: full decode"
    decode_n "$CLIP" 0 "$D/decode.i420" || echo "full decode failed"
    md5sum "$D/decode.i420" | tee "$D/md5.txt"
    W=$(gst-discoverer-1.0 "$CLIP" 2>/dev/null | sed -n 's/.*Width: \([0-9]*\).*/\1/p' | head -1)
    H=$(gst-discoverer-1.0 "$CLIP" 2>/dev/null | sed -n 's/.*Height: \([0-9]*\).*/\1/p' | head -1)
    if [ -n "$W" ] && [ -n "$H" ]; then
        python3 - "$D/decode.i420" "$W" "$H" > "$D/frame-md5.txt" <<'PY'
import hashlib, sys
path, w, h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
fs = w * h * 3 // 2
with open(path, "rb") as f:
    i = 0
    while True:
        b = f.read(fs)
        if len(b) < fs:
            break
        print("frame %4d md5 %s" % (i, hashlib.md5(b).hexdigest()))
        i += 1
PY
        echo "${W}x${H}, $(wc -l < "$D/frame-md5.txt") frames" | tee -a "$D/md5.txt"
    fi

    # ftrace: call graph of rkvdec for a 2-picture decode, and MMIO writes if traced
    T=/sys/kernel/tracing
    [ -d $T ] || T=/sys/kernel/debug/tracing
    if [ -w $T/current_tracer ]; then
        echo 0 > $T/tracing_on
        echo > $T/trace
        if grep -q function_graph $T/available_tracers; then
            echo 'rkvdec*' > $T/set_graph_function 2>/dev/null || true
            echo 'rkvdec*' > $T/set_ftrace_filter 2>/dev/null || true
            echo function_graph > $T/current_tracer
        fi
        [ -d $T/events/rwmmio ] && echo 1 > $T/events/rwmmio/enable
        echo 1 > $T/tracing_on
        decode_n "$CLIP" 2 "" || true
        echo 0 > $T/tracing_on
        cat $T/trace > "$D/ftrace-2pics.txt"
        [ -d $T/events/rwmmio ] && echo 0 > $T/events/rwmmio/enable
        echo nop > $T/current_tracer
        echo > $T/set_graph_function 2>/dev/null || true
        echo > $T/set_ftrace_filter 2>/dev/null || true
    else
        echo "no ftrace" > "$D/ftrace-2pics.txt"
    fi
done

clocks_and_power after
hold_awake auto
dmesg > "$OUT/dmesg-after.txt"

tar czf "$OUT.tar.gz" "$OUT"
say "done: $OUT.tar.gz ($(stat -c%s "$OUT.tar.gz") bytes). Send this file."
