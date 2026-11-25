#!/usr/bin/env bash
# run-remote.sh - run the regression suite on a board over SSH from the host:
# copy tools/regress there, run it, copy the results back.
#
#   ./run-remote.sh [--os windows|linux] [--results DIR] [--no-streams] HOST [SUITE ARGS...]
#
#   ./run-remote.sh woo@tspi3m.lan -Quick
#   ./run-remote.sh woo@cm5io.lan -Stress -StressSeconds 120
#   ./run-remote.sh --os linux root@rock4d.lan --quick
#
# HOST is anything ssh accepts. Windows or Linux is told apart by asking the
# remote shell for $PSVersionTable (the Windows boards run OpenSSH with
# PowerShell as the shell); --os skips the question. SUITE ARGS go to
# regress.ps1 (Windows) or regress.sh (Linux) unchanged.
#
# Windows: the suite goes to C:\RK3576-regress\suite (replaced on every run),
# results to C:\RK3576-regress\out\<time>. Linux: /var/tmp/rk3576-regress-suite
# and /var/tmp/rk3576-regress/<time>, run with sudo -n when that works
# without a password. On the host the results land in DIR (default
# tools/regress/results/<host>-<time>). Exit code: the suite's (0 no FAIL,
# 1 a FAIL, 2 could not run), or 3 when SSH or the copy failed.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OS=
RES=
STREAMS=1
while [ $# -gt 0 ]; do
    case "$1" in
        --os) OS=$2; shift 2 ;;
        --results) RES=$2; shift 2 ;;
        --no-streams) STREAMS=0; shift ;;
        -h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --) shift; break ;;
        -*) echo "unknown option $1 (options come before HOST)" >&2; exit 2 ;;
        *) break ;;
    esac
done
[ $# -ge 1 ] || { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }
HOST=$1; shift
STAMP=$(date +%Y%m%d-%H%M%S)
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=30)

if [ -z "$OS" ]; then
    # PowerShell prints a number; a POSIX shell prints ".PSVersion.Major"
    v=$(ssh "${SSH_OPTS[@]}" "$HOST" 'echo $PSVersionTable.PSVersion.Major' 2>/dev/null) || { echo "cannot reach $HOST over ssh" >&2; exit 3; }
    v=$(printf '%s' "$v" | tr -d '\r\n ')
    case "$v" in [0-9]*) OS=windows ;; *) OS=linux ;; esac
fi
echo "host $HOST: $OS"

# a copy of the suite with a VERSION file naming the commit it came from
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/suite"
cp -r "$HERE/regress.ps1" "$HERE/regress.sh" "$HERE/boards" "$HERE/streams.txt" "$STAGE/suite/"
desc=$(git -C "$HERE" describe --always --dirty 2>/dev/null || echo unknown)
echo "woa-rk3576 tools/regress $desc" > "$STAGE/suite/VERSION"
if [ "$OS" = windows ]; then
    [ -d "$HERE/bin" ] && cp -r "$HERE/bin" "$STAGE/suite/"
    if [ "$STREAMS" = 1 ]; then
        "$HERE/fetch-streams.sh" >/dev/null || echo "warning: some streams could not be fetched; their checks will be SKIP" >&2
        [ -d "$HERE/streams" ] && cp -r "$HERE/streams" "$STAGE/suite/"
    fi
fi

[ -z "$RES" ] && RES="$HERE/results/$(printf '%s' "$HOST" | tr -c 'A-Za-z0-9._-' '_')-$STAMP"
mkdir -p "$(dirname "$RES")"

if [ "$OS" = windows ]; then
    ssh "${SSH_OPTS[@]}" "$HOST" "Remove-Item -Recurse -Force C:\\RK3576-regress\\suite -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force -Path C:\\RK3576-regress\\out | Out-Null" || exit 3
    scp -q -r "${SSH_OPTS[@]}" "$STAGE/suite" "$HOST:C:/RK3576-regress/suite" || { echo "copy to $HOST failed" >&2; exit 3; }
    args=
    for a in "$@"; do
        case "$a" in *[\ \']*) args="$args '${a//\'/\'\'}'" ;; *) args="$args $a" ;; esac
    done
    ssh "${SSH_OPTS[@]}" "$HOST" "powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\\RK3576-regress\\suite\\regress.ps1 -Out C:\\RK3576-regress\\out\\$STAMP$args; exit \$LASTEXITCODE"
    rc=$?
    scp -q -r "${SSH_OPTS[@]}" "$HOST:C:/RK3576-regress/out/$STAMP" "$RES" || { echo "copy of the results failed" >&2; [ "$rc" -le 2 ] && rc=3; }
else
    ssh "${SSH_OPTS[@]}" "$HOST" "rm -rf /var/tmp/rk3576-regress-suite" || exit 3
    scp -q -r "${SSH_OPTS[@]}" "$STAGE/suite" "$HOST:/var/tmp/rk3576-regress-suite" || { echo "copy to $HOST failed" >&2; exit 3; }
    args=
    for a in "$@"; do args="$args $(printf '%q' "$a")"; done
    ssh "${SSH_OPTS[@]}" "$HOST" "S=; if [ \"\$(id -u)\" != 0 ] && sudo -n true 2>/dev/null; then S='sudo -n'; fi; \$S bash /var/tmp/rk3576-regress-suite/regress.sh --out /var/tmp/rk3576-regress/$STAMP$args"
    rc=$?
    scp -q -r "${SSH_OPTS[@]}" "$HOST:/var/tmp/rk3576-regress/$STAMP" "$RES" || { echo "copy of the results failed" >&2; [ "$rc" -le 2 ] && rc=3; }
fi

if [ -f "$RES/results.md" ]; then
    echo
    echo "results: $RES/results.md"
fi
exit "$rc"
