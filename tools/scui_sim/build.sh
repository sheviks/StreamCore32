#!/bin/bash
# Desktop preview of the StreamCore32 e-paper UI (no hardware needed).
#   ./build.sh                 → builds ./scui_sim
#   ./scui_sim out             → renders the pages into out/*.pgm + runs touch checks
#   python3 topng.py out 2     → converts to 2x PNGs (needs Pillow; on Windows: python)
set -e
cd "$(dirname "$0")"
C=../../components             # all components
GFX=$C/Adafruit-GFX
UI=$C/eink_ui
mkdir -p gen out
# python3 on Linux / macOS, python on Windows
PY=$(command -v python3 || command -v python || command -v py)
[ -z "$PY" ] && { echo "Python 3 not found"; exit 1; }
"$PY" $C/eink_vg/tools/svg_folder_to_vg_header.py \
  $C/eink_vg/assets/svg gen/vg_assets.h --box 64x64 > /dev/null
g++ -std=c++17 -O1 -g -w -ffunction-sections -Wl,--gc-sections ${SIM_CFLAGS} -Istubs -I. -Igen \
  -I$UI -I$UI/include -I$UI/elements -I$UI/pages \
  -I$C/eink_vg/include -I$C/sc_app/include -I$GFX \
  sim_main.cpp $GFX/Adafruit_GFX.cpp stubs/itoa_stub.cpp stubs/print_stub.cpp -o scui_sim
echo "built ./scui_sim"
