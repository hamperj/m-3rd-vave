#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# ./build.sh   M-3rd-VAVE as an fm1-dock module: build/m3rdvave.dock (deploy it with fm1-dock's tools/dock.py deploy)
# Needs fm1-dock's tree (FM1_DOCK, default ../fm1-dock next to this repository) and its JieLi toolchain
# (JIELI_TOOLCHAIN): see fm1-dock's README. On macOS the toolchain runs in Docker.
set -eu
cd "$(dirname "$0")"
HERE=$PWD
HB="${FM1_DOCK:-$(cd .. && pwd)/fm1-dock}"
[ -f "$HB/tools/hbmod.py" ] || { echo "no fm1-dock at $HB (set FM1_DOCK)"; exit 1; }
mkdir -p build
export HB_MOUNT="$HERE"
(cd "$HB" && python3 tools/hbmod.py "$HERE/m3rdvave") | tail -1
cp "$HB/build/modules/m3rdvave.dock" build/
ls build/m3rdvave.dock
