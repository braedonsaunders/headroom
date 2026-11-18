#!/bin/zsh
# usage: eval.sh label [ENV=VAL ...]  — runs the full GuitarSet key benchmark in 8 shards and prints a summary
lab=$1; shift
ROOT_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"
D="$ROOT_DIR/work/midi/dl/gset"
KS="${KEY_STUDY_DIR:-/tmp/key_study}"
for i in 0 1 2 3 4 5 6 7; do (cd "$ROOT_DIR/app" && env "$@" $KS/lb --key-eval2 "$D" $i/8 > $KS/out_${lab}_$i.txt 2>&1) & done; wait
cat $KS/out_${lab}_*.txt | awk -v L="$lab" '/^RAW/ { k=$2; e[k]+=$3; w[k]+=$4; te[k]+=$5; tn[k]+=$6; c[k]+=$7; n[k]+=$8 }
 END { printf "%-10s", L; for (k in n) printf "  %s: exact %5.1f%% weighted %5.1f%% time-exact %5.1f%% changes %.2f (n=%d)", k, 100*e[k]/n[k], 100*w[k]/n[k], 100*te[k]/tn[k], c[k]/n[k], n[k]; printf "   ALL exact %.1f%%\n", 100*(e["comp"]+e["solo"])/(n["comp"]+n["solo"]) }'
