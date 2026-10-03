#!/usr/bin/env bash
# Regenerate the lifted C from the XBE: disasm -> func_id -> abi -> recomp.
#
#   tools/regen.sh
#
# Environment:
#   XBOXRECOMP_DIR  toolkit            (default: the vendored xboxrecomp/)
#   NFSU1_XBE       default.xbe        (default: /root/nfsu1x/game/default.xbe)
#   NFSU1_GEN_DIR   output directory   (default: /root/nfsu1x/gen)
#   LIFT_ONLY=1     skip disasm/func_id/abi (enough after editing
#                   recomp_manual.c; seeds need the full run)
#
# Seeds (config/seed_functions.json) are entry points the static pass cannot
# see -- thread start routines and indirect-call targets found at runtime.
# Grow them with:  python3 -m tools.seed_from_log run.log default.xbe \
#                    --functions tools/disasm/output/functions.json \
#                    --seeds <this repo>/config/seed_functions.json
# and review every addition: a seed inside a function splits it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TK="${XBOXRECOMP_DIR:-$HERE/xboxrecomp}"
XBE="${NFSU1_XBE:-/root/nfsu1x/game/default.xbe}"
GEN="${NFSU1_GEN_DIR:-/root/nfsu1x/gen}"

cd "$TK"
if [ "${LIFT_ONLY:-0}" != "1" ]; then
    python3 -m tools.disasm "$XBE" --seed-functions "$HERE/config/seed_functions.json"
    python3 -m tools.func_id "$XBE"
    python3 -m tools.abi_analysis "$XBE"
fi
mkdir -p "$GEN"
# recomp_types.h is written only when absent, so a stale copy would hide
# register-model changes in the toolkit. Nothing here edits it by hand.
rm -f "$GEN/recomp_types.h"
python3 -m tools.recomp "$XBE" --game-name "NFS Underground" --all --split 250 \
    --gen-dir "$GEN" --exclude-manual "$HERE/src/recomp_manual.c" \
    --mmio-sections DSOUND,XPP
