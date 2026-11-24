#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# M-3rd-VAVE on this computer: the engine (every preset rendered and checked, the two cores against one, the voice
# cap, the cost) into build/host/wav/*.wav, then the panel (its own ui() / draw() on fm1-dock's canvas and fonts) into
# build/host/m3rdvave-*.png. Needs fm1-dock's tree for the panel (FM1_DOCK, default ../fm1-dock) and Pillow.
set -e
cd "$(dirname "$0")/.."
HB="${FM1_DOCK:-$(cd .. && pwd)/fm1-dock}"
mkdir -p build/host/wav
cc -O2 -Wall -Wextra -ffp-contract=off -Im3rdvave/eng -o build/host/engine m3rdvave/eng/test_engine.c
build/host/engine build/host/wav
cc -O2 -Wall -Wno-unused-function -ffp-contract=off -I"$HB/firmware/dock" -I"$HB/modules/lib" -I"$HB/firmware/src" \
    -I"$HB/firmware/gen" -Im3rdvave/eng -Im3rdvave -o build/host/panel m3rdvave/eng/test_ui.c
build/host/panel build/host/m3rdvave
python3 - <<'PY'
from pathlib import Path
from PIL import Image
for p in sorted(Path("build/host").glob("m3rdvave-*.ppm")):
    Image.open(p).resize((480, 480), Image.NEAREST).save(p.with_suffix(".png"))
    p.unlink()
PY
