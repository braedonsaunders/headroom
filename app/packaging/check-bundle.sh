#!/usr/bin/env bash
# shellcheck disable=SC2053  # the unquoted right-hand side of == is the glob match this script relies on
# Verifies a staged Headroom release tree before it is signed / packaged.
#   check-bundle.sh <staged-root> [--strict]
# Fails (exit 1) when the tree contains
#   * anything bundle-denylist.txt lists: untracked local files (extracted/), DI recordings (samples/di), research
#     scratch (work/), audio renders (samples/processed), tool litter;
#   * any file no line of content-manifest.txt allows;
#   * shipped assets (.svg, .json, .rig, .desktop, .xml) matching the denylist's text pattern, if it sets one.
set -euo pipefail
shopt -s nocasematch

root=${1:?usage: check-bundle.sh <staged-root> [--strict]}
strict=0
[[ ${2:-} == --strict ]] && strict=1
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
manifest="$here/content-manifest.txt"
denylist="$here/bundle-denylist.txt"
[[ -d $root ]] || { echo "check-bundle: no such directory: $root" >&2; exit 2; }

allow=()
while IFS= read -r line || [[ -n $line ]]; do
    line=${line%%#*}
    line=$(printf '%s' "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    [[ -n $line ]] && allow+=("$line")
done <"$manifest"

deny_path=()
deny_ext=()
deny_text=''
[[ -f $denylist ]] || { echo "check-bundle: missing $denylist" >&2; exit 2; }
while IFS= read -r line || [[ -n $line ]]; do
    [[ $line == \#* || -z ${line//[[:space:]]/} ]] && continue
    key=${line%%:*}; val=$(printf '%s' "${line#*:}" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    case $key in
        path) deny_path+=("$val") ;;
        ext)  read -r -a _e <<<"$val"; deny_ext+=("${_e[@]}") ;;
        text) deny_text=$val ;;
    esac
done <"$denylist"

fail=0 warn=0 count=0
while IFS= read -r -d '' f; do
    rel=${f#"$root"/}
    count=$((count + 1))
    for p in ${deny_path[@]+"${deny_path[@]}"}; do
        if [[ $rel == $p ]]; then echo "DENY  $rel   (matches $p)"; fail=1; continue 2; fi
    done
    ext=${rel##*.}
    for e in ${deny_ext[@]+"${deny_ext[@]}"}; do
        if [[ $ext == "$e" ]]; then echo "DENY  $rel   (denied file type .$e)"; fail=1; continue 2; fi
    done
    ok=0
    for p in ${allow[@]+"${allow[@]}"}; do
        if [[ $rel == $p ]]; then ok=1; break; fi
    done
    if (( ! ok )); then echo "UNLISTED  $rel"; fail=1; continue; fi
    case $rel in
        *.svg|*.json|*.rig|*.desktop|*.xml)
            if [[ -n $deny_text ]] && grep -qiE "$deny_text" "$f" 2>/dev/null; then
                warn=$((warn + 1))
                fail=1
                (( warn <= 10 )) && echo "DENY  $rel matches the denylist text pattern"
            fi ;;
    esac
done < <(find "$root" \( -type f -o -type l \) -print0)

(( warn > 10 )) && echo "DENY  … $((warn - 10)) more files match the denylist text pattern"
echo "check-bundle: $count files, $warn text-pattern matches, $([[ $fail == 1 ]] && echo FAILED || echo OK)"
(( fail )) && exit 1
(( strict && warn )) && exit 1
exit 0
