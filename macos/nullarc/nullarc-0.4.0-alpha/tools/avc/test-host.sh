#!/bin/bash
# SPDX-License-Identifier: MIT
set -euo pipefail
src=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/phx-avc-host.XXXXXXXX")
trap 'rm -rf "$tmp"' EXIT
flags=(-O1 -g -Wall -Wextra -Werror -Wconversion -Wpedantic -fsanitize=address,undefined -fno-sanitize-recover=all)
"${CC:-clang}" -std=c11 "${flags[@]}" -c "$src/avd_h264.c" -o "$tmp/avd.o"
"${CC:-clang}" -std=c11 "${flags[@]}" "$src/h264.c" "$tmp/avd.o" -o "$tmp/idr-test"
"${CXX:-clang++}" -std=c++17 "${flags[@]}" "$src/avc_stream_parser.cpp" "$src/avc_stream_test.cpp" "$tmp/avd.o" -o "$tmp/stream-test"

for size in 864x480 1920x1080; do
    "${FFMPEG:-ffmpeg}" -nostdin -hide_banner -loglevel error -f lavfi -i "testsrc2=size=$size:rate=30" \
        -frames:v 25 -c:v libx264 -profile:v baseline -level 4.0 -threads 1 \
        -x264-params 'ref=1:bframes=0:weightp=0:aud=1:slices=4:keyint=12:min-keyint=12:scenecut=0' \
        -f h264 "$tmp/$size.h264"
done
"$tmp/stream-test" "$tmp/864x480.h264" "$tmp/1920x1080.h264"

fixtures="$src/../vcn"
reference=${PHX_REFERENCE_IDR:-"$src/fixtures/libdrm-idr.h264"}
"$tmp/idr-test" "$reference" "$fixtures/vcn_quadrants.h264" "$fixtures/fixture-2048x1152.h264" \
    "$fixtures/fixture-2560x1440.h264" "$tmp/1920x1080.h264"
