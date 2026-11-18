#!/usr/bin/env bash
# macOS release: universal (arm64 + x86_64) Headroom.app -> hardened-runtime signature -> .dmg -> notarization.
#
#   app/packaging/macos/build-release.sh [options]
#     --identity ID         codesign identity (default: $HEADROOM_SIGN_IDENTITY, else "-" = ad-hoc)
#                           Distribution needs "Developer ID Application: <Name> (<TEAMID>)".
#     --notary-profile P    notarytool keychain profile (default: $HEADROOM_NOTARY_PROFILE)
#                           create once: xcrun notarytool store-credentials P --apple-id … --team-id … --password <app-specific>
#     --submit              actually submit to Apple's notary service and staple (otherwise the commands are printed)
#     --plugins             also build, sign and package the AU + VST3 plugins (.pkg into /Library/Audio/Plug-Ins)
#     --build-dir DIR       default app/build-port-pkg      --out DIR   default <build-dir>/dist
#     --jobs N              default 3 (always run under nice)
#     --no-build            reuse an existing build dir
#
# Never launches the app. Without --submit nothing leaves this machine.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
app_dir=$(cd "$here/../.." && pwd)
identity=${HEADROOM_SIGN_IDENTITY:--}
notary=${HEADROOM_NOTARY_PROFILE:-}
submit=0 plugins=0 build=1 jobs=3
build_dir="$app_dir/build-port-pkg"
out=""
while (($#)); do
    case $1 in
        --identity) identity=$2; shift ;;
        --notary-profile) notary=$2; shift ;;
        --submit) submit=1 ;;
        --plugins) plugins=1 ;;
        --build-dir) build_dir=$2; shift ;;
        --out) out=$2; shift ;;
        --jobs) jobs=$2; shift ;;
        --no-build) build=0 ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
out=${out:-$build_dir/dist}
stage="$build_dir/stage"
version=$(sed -n 's/^set(RIG_PACKAGE_VERSION "\([^"]*\)".*/\1/p' "$app_dir/packaging/Install.cmake")
dmg="$out/Headroom-$version-macOS-universal.dmg"
developer_id=0
if [[ $identity == "Developer ID Application:"* ]]; then
    developer_id=1
elif [[ $identity != "-" ]] && security find-identity -v -p codesigning | grep -F "$identity" | grep -q "Developer ID Application"; then
    developer_id=1
fi

# Compiler-level optimisation (docs/CIRCUIT_ENGINE.md §10.54): the arm64 profile (both slices; AUTO falls back to no
# profile if this clang cannot read it) and ThinLTO on the shipped binaries. Both are output-identical to the plain build.
opt="-DHEADROOM_PGO=AUTO -DHEADROOM_LTO=PRODUCTS"

step() { printf '\n== %s\n' "$*"; }

if ((build)); then
    step "configure + build (universal, release bundle) in $build_dir"
    cmake -S "$app_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
        -DRIG_RELEASE_BUNDLE=ON -DHEADROOM_SIGN_IDENTITY=- $opt >/dev/null
    nice ninja -C "$build_dir" -j "$jobs" rig_studio
fi

step "stage (cmake --install) -> $stage"
rm -rf "$stage"
cmake --install "$build_dir" --prefix "$stage" >/dev/null
app="$stage/Headroom.app"
lipo -archs "$app/Contents/MacOS/Headroom"

step "content check"
"$app_dir/packaging/check-bundle.sh" "$app"

sign() { # sign <path> <entitlements>
    local args=(--force --options runtime --entitlements "$2" --sign "$identity")
    ((developer_id)) && args+=(--timestamp)
    codesign "${args[@]}" "$1"
}

step "codesign ($identity, hardened runtime)"
# Inside-out: nested code first (none today besides the main executable; frameworks/helpers would go here).
while IFS= read -r -d '' nested; do sign "$nested" "$here/Plugin.entitlements"; done \
    < <(find "$app/Contents" \( -name '*.dylib' -o -name '*.framework' -o -name '*.bundle' \) -print0)
sign "$app" "$here/Headroom.entitlements"
codesign --verify --strict --deep --verbose=2 "$app"
codesign -dv --verbose=4 "$app" 2>&1 | grep -E '^(Identifier|Format|CodeDirectory|Authority|TeamIdentifier|Runtime|Timestamp|Signature)' || true
codesign -d --entitlements :- "$app" 2>/dev/null | grep -oE 'com\.apple\.[a-z.-]+' || true
step "Gatekeeper assessment (rejected until Developer ID + notarized)"
spctl -a -vv -t exec "$app" 2>&1 || true

if ((plugins)); then
    step "plugins: universal AU + VST3"
    pb="$build_dir/plugin"
    cmake -S "$app_dir/plugin" -B "$pb" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DHEADROOM_SIGN_IDENTITY=- $opt >/dev/null
    nice ninja -C "$pb" -j "$jobs" plugins
    proot="$build_dir/pkgroot"
    rm -rf "$proot"; mkdir -p "$proot/Library/Audio/Plug-Ins/Components" "$proot/Library/Audio/Plug-Ins/VST3"
    ditto "$(find "$pb" -maxdepth 3 -name Headroom.component -type d | head -1)" "$proot/Library/Audio/Plug-Ins/Components/Headroom.component"
    ditto "$(find "$pb" -maxdepth 4 -name Headroom.vst3 -type d | head -1)" "$proot/Library/Audio/Plug-Ins/VST3/Headroom.vst3"
    for b in "$proot"/Library/Audio/Plug-Ins/*/Headroom.*; do
        "$app_dir/packaging/check-bundle.sh" "$b" || true   # plugin bundles carry ui/ under Contents/Resources
        sign "$b" "$here/Plugin.entitlements"; codesign --verify --strict --verbose=1 "$b"
    done
    mkdir -p "$out"
    pkg_id=${HEADROOM_INSTALLER_IDENTITY:-}
    pkgbuild --root "$proot" --identifier com.headroom.plugins --version "$version" --install-location / \
        ${pkg_id:+--sign "$pkg_id"} "$out/Headroom-Plugins-$version.pkg"
    echo "plugin installer: $out/Headroom-Plugins-$version.pkg (sign with a 'Developer ID Installer' identity via HEADROOM_INSTALLER_IDENTITY)"
fi

step "dmg"
mkdir -p "$out"
droot="$build_dir/dmgroot"
rm -rf "$droot" "$dmg"; mkdir -p "$droot"
ditto "$app" "$droot/Headroom.app"
ln -s /Applications "$droot/Applications"
hdiutil create -quiet -volname "Headroom $version" -srcfolder "$droot" -fs HFS+ -format UDZO -ov "$dmg"
rm -rf "$droot"
((developer_id)) && codesign --force --timestamp --sign "$identity" "$dmg"
du -h "$dmg"

step "notarization"
submit_cmd=(xcrun notarytool submit "$dmg" --keychain-profile "${notary:-<profile>}" --wait)
staple_cmd=(xcrun stapler staple "$dmg")
if ((submit)); then
    ((developer_id)) || { echo "refusing to submit: identity is not a Developer ID Application certificate" >&2; exit 1; }
    [[ -n $notary ]] || { echo "refusing to submit: no --notary-profile" >&2; exit 1; }
    "${submit_cmd[@]}"
    "${staple_cmd[@]}"
    xcrun stapler validate "$dmg"
    spctl -a -vv -t open --context context:primary-signature "$dmg"
else
    echo "dry run — to notarize run:"
    printf '  %q' "${submit_cmd[@]}"; echo
    printf '  %q' "${staple_cmd[@]}"; echo
    ((developer_id)) || echo "  (first sign with a 'Developer ID Application' identity: --identity \"Developer ID Application: …\")"
fi
