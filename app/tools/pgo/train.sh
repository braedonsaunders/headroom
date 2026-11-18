#!/usr/bin/env bash
# PGO training (docs/CIRCUIT_ENGINE.md §10.54): builds an instrumented circuit_cli, plays held-out DI through every bound
# circuit unit, and writes the merged profile to app/pgo/headroom-<arch>.profdata (or $OUT).
#
#   app/tools/pgo/train.sh [build-dir]        default build dir: app/build-pgo-train (deleted afterwards unless KEEP=1)
#   env: JOBS (parallel units, default 4), TRAIN_DI (pre-made clips), CXX, CMAKE_ARGS (e.g. "-DCMAKE_CXX_FLAGS=-march=x86-64-v3" on x86), OUT, KEEP
#
# The corpus: every unit in unitBindings() (CircuitUnit.cpp) plays two clips through the unit path (`bench --real`
# with CE_REAL=synthetic: the clip, then the synthetic hard suite of HF tones, a sweep and low bursts); the heavy amps
# play all six; each unit also plays one clip on the interpreter (CE_NOCODEGEN=1). The clips are GuitarSet comping and solos (tools/pgo/training_di.py), not the four DIs `bench --real`
# times, so the measurement is held out from the training. Counts weight themselves: a unit that costs 1.5 cores
# contributes ~30x the counts of a light pedal, and every unit's specialised kernels are covered (none is "cold").
#
# Retrain after big engine changes (new device laws, Newton / solver paths, a batch of new units); a stale profile stays
# output-identical but loses its gain (docs §10.54, "When to retrain"). After retraining, gate the profile: `bench --real`
# hashes of all bound units must equal the HEADROOM_PGO=OFF build's (work/circuit-netlists/pgo/scripts/hashes.sh), then
# run the circuit tests.
set -euo pipefail
APP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
B="${1:-$APP/build-pgo-train}"
JOBS="${JOBS:-4}"
ARCH="$(uname -m)"; [ "$ARCH" = aarch64 ] && ARCH=arm64
OUT="${OUT:-$APP/pgo/headroom-$ARCH.profdata}"
if [ "$(uname)" = Darwin ]; then PROFDATA=(xcrun llvm-profdata); else
    PROFDATA=("$(command -v llvm-profdata || command -v llvm-profdata-20 || echo "$(dirname "${CXX:-clang++}")/llvm-profdata")"); fi

if [ ! -x "$B/circuit_cli" ]; then
    # shellcheck disable=SC2086
    cmake -S "$APP" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DHEADROOM_PGO=GENERATE -DRIG_BUILD_APP=OFF \
        ${CXX:+-DCMAKE_CXX_COMPILER=$CXX} ${CMAKE_ARGS:-} >/dev/null
    nice -n 10 cmake --build "$B" --target circuit_cli
fi
RAW="$B/pgo-raw"; rm -rf "$RAW"; mkdir -p "$RAW"
DI="${TRAIN_DI:-$B/pgo-di}" # TRAIN_DI: clips made elsewhere (workers carry no app/samples/di)
[ -n "${TRAIN_DI:-}" ] || python3 -I "$APP/tools/pgo/training_di.py" "$APP/samples/di" "$DI"

LIB="$APP/src/circuit/library"
# id → netlist file, from the binding table and the embedded library index
UNITS=$(python3 -I - "$APP/src/circuit/CircuitUnit.cpp" "$LIB/EmbeddedLibrary.hpp" <<'PY'
import re, sys
emb = dict(re.findall(r'^    \{"([a-z0-9-]+)", "([A-Za-z0-9._-]+\.json)"', open(sys.argv[2]).read(), re.M))
src = open(sys.argv[1]).read()
body = src[src.index("unitBindings()"):]; body = body[:body.index("return b;")]
for uid, name in re.findall(r'\{(\d+), "([a-z0-9-]+)"', body): print(uid, emb[name])
PY
)
HEAVY=" 134000 81000 151000 122000 163000 49000 16000 34000 56000 30000 38000 "
run() { # id file
    local id=$1 f=$2 clips="train0.wav train1-driven.wav"
    [[ "$HEAVY" == *" $id "* ]] && clips="train0.wav train0-driven.wav train1.wav train1-driven.wav train2.wav train2-driven.wav"
    for c in $clips; do
        LLVM_PROFILE_FILE="$RAW/$id-%p.profraw" CE_REAL=synthetic CE_REALREPS=1 nice -n 10 \
            "$B/circuit_cli" bench "$LIB/$f" --table "$id" --real --wav "$DI/$c" >/dev/null 2>&1 || echo "train: $id $c failed" >&2
    done
    # the interpreter too (CE_NOCODEGEN): the plugin builds and edited / imported circuits run it, and untrained it would
    # be profiled cold (optimised for size)
    LLVM_PROFILE_FILE="$RAW/$id-i%p.profraw" CE_NOCODEGEN=1 CE_REAL=synthetic CE_REALREPS=1 nice -n 10 \
        "$B/circuit_cli" bench "$LIB/$f" --table "$id" --real --wav "$DI/train2.wav" >/dev/null 2>&1 || echo "train: $id interpreter failed" >&2
}
export -f run; export B RAW DI LIB HEAVY
echo "$UNITS" | xargs -P "$JOBS" -L 1 bash -c 'run "$0" "$1"'
mkdir -p "$(dirname "$OUT")"
"${PROFDATA[@]}" merge -o "$OUT" "$RAW"/*.profraw
echo "profile: $OUT ($(du -h "$OUT" | cut -f1), $(ls "$RAW" | wc -l | tr -d ' ') runs)"
[ "${KEEP:-0}" = 1 ] || [ -n "${1:-}" ] || rm -rf "$B"
