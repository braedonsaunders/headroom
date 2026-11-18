#!/bin/zsh
# Re-bundle the unit illustrations after any redraw: sync (strips floor shadows/feet/legs), measure each face's
# real outline in headless Chrome, save tight crops to unit_art_crop.json, and sync again with the crops applied.
#   refresh_unit_art.sh              measure every face (rewrites unit_art_crop.json)
#   refresh_unit_art.sh 56000 ...    measure only these table ids (their entries are merged into unit_art_crop.json)
#   refresh_unit_art.sh --prune ...  also delete superseded hashed files nothing in the new index references
set -e
T="${0:A:h}"; APP="${T:h}"
PRUNE=(); ONLY=()
for a in "$@"; do if [[ "$a" == --prune ]]; then PRUNE=(--prune); elif [[ "$a" == <-> ]]; then ONLY+=("$a"); else print -u2 "usage: ${0:t} [--prune] [table_id ...]"; exit 2; fi; done
python3 -I "$T/sync_unit_art.py" --quiet
cd "$APP/ui"; python3 -m http.server 8791 --bind 127.0.0.1 >/dev/null 2>&1 & SRV=$!
trap 'kill $SRV 2>/dev/null; playwright-cli -s=ua-measure close >/dev/null 2>&1 || true; rm -rf "$APP/ui/.playwright-cli"' EXIT
sleep 1
playwright-cli -s=ua-measure open about:blank >/dev/null 2>&1
TMP=$(mktemp)
CODE=$(cat "$T/measure_unit_art.js")
CODE=${CODE/"const ONLY = [];"/"const ONLY = [${(j:,:)ONLY}];"}
playwright-cli -s=ua-measure run-code "$CODE" > "$TMP" 2>&1
python3 -I -c '
import json, re, sys
t = open(sys.argv[1]).read(); m = re.search(r"### Result\s*\n(\".*\")\s*\n", t)
d = json.loads(json.loads(m.group(1)))
new = {k: {"k": v["k"], "vb": v["vb"], "jacks": v.get("jacks", [])} for k, v in d.items()}
if sys.argv[3] == "1":   # a subset was measured: keep the crops of the rest
    try:
        old = json.load(open(sys.argv[2]))
    except Exception:
        old = {}
    old.update(new); new = old
json.dump(new, open(sys.argv[2], "w"), indent=0)
print(len(d), "faces measured,", sum(1 for v in d.values() if v.get("jacks")), "with jacks found")' "$TMP" "$T/unit_art_crop.json" "$(( ${#ONLY} > 0 ))"
rm -f "$TMP"
python3 -I "$T/sync_unit_art.py" $PRUNE
