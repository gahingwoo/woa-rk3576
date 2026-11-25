#!/usr/bin/env bash
# fetch-streams.sh - download the decode set of streams.txt into streams/
# (FFmpeg's fate-suite) and check each file's SHA-256. Files already there
# with the right hash are kept. run-remote.sh calls this before copying the
# suite to a Windows board.
#
#   ./fetch-streams.sh [DIR]
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
DIR=${1:-$HERE/streams}
BASE=${FATE_SUITE:-https://fate-suite.ffmpeg.org}
mkdir -p "$DIR"
bad=0
while read -r codec name path sha frames md5; do
    case "$codec" in ''|'#'*) continue ;; esac
    f="$DIR/$(basename "$path")"
    if [ -f "$f" ] && [ "$(sha256sum < "$f" | cut -d' ' -f1)" = "$sha" ]; then
        echo "ok      $codec $name"
        continue
    fi
    if ! curl -sfL --retry 3 -o "$f.part" "$BASE/$path"; then
        echo "FAILED  $codec $name: download"; rm -f "$f.part"; bad=$((bad + 1)); continue
    fi
    if [ "$(sha256sum < "$f.part" | cut -d' ' -f1)" != "$sha" ]; then
        echo "FAILED  $codec $name: SHA-256 differs"; rm -f "$f.part"; bad=$((bad + 1)); continue
    fi
    mv "$f.part" "$f"
    echo "fetched $codec $name ($frames frames, md5 $md5)"
done < "$HERE/streams.txt"
exit "$bad"
