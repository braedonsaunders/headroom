#!/usr/bin/env bash
# Linux packages from one release build: .deb + .tar.gz (CPack) and an AppImage (linuxdeploy + GTK plugin).
#
#   app/packaging/linux/build-appimage.sh [--build-dir DIR] [--out DIR] [--jobs N] [--no-build] [--no-appimage]
#
# Build deps (Ubuntu 24.04): build-essential cmake ninja-build pkg-config libgtk-3-dev libwebkit2gtk-4.1-dev
#   libasound2-dev libjack-jackd2-dev libsecret-1-dev (plus dpkg-dev for the .deb's shlibdeps) and, for the
#   AppImage, wget/curl + file + FUSE or APPIMAGE_EXTRACT_AND_RUN=1.
#
# WebKitGTK caveat: WebKitGTK runs helper processes (WebKitWebProcess, WebKitNetworkProcess) from a path compiled
# into libwebkit2gtk, and links against the system's GL/Mesa stack. linuxdeploy-plugin-gtk bundles GTK, but a
# fully relocatable WebKitGTK AppImage needs those helpers bundled and WEBKIT_EXEC_PATH pointed at them (the
# plugin's AppRun hook does not do this). Until that is solved the AppImage is built with WebKitGTK *excluded*
# (it uses the host's libwebkit2gtk-4.1, present on mainstream desktops); the .deb declares it as a dependency.
# Flatpak (packaging/flatpak) is the clean answer: the GNOME runtime ships WebKitGTK.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
app_dir=$(cd "$here/../.." && pwd)
build_dir="$app_dir/build-linux-pkg"
out="" jobs=$(( $(nproc) > 2 ? $(nproc) - 2 : 1 )) build=1 appimage=1
while (($#)); do
    case $1 in
        --build-dir) build_dir=$2; shift ;;
        --out) out=$2; shift ;;
        --jobs) jobs=$2; shift ;;
        --no-build) build=0 ;;
        --no-appimage) appimage=0 ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
out=${out:-$build_dir/dist}
mkdir -p "$out"

if ((build)); then
    cmake -S "$app_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release -DRIG_RELEASE_BUNDLE=ON -DCMAKE_INSTALL_PREFIX=/usr
    nice ninja -C "$build_dir" -j "$jobs"
fi
grep -q '^rig_studio' <(ninja -C "$build_dir" -t targets all 2>/dev/null | sed 's/:.*//') ||
    { echo "no rig_studio target in $build_dir (the Linux shell isn't built yet)" >&2; exit 1; }

echo "== .deb / .tar.gz (CPack)"
(cd "$build_dir" && cpack -G "DEB;TGZ" -B "$out" >/dev/null)
ls -1 "$out"/*.deb "$out"/*.tar.gz

echo "== stage AppDir + content check"
appdir="$build_dir/AppDir"
rm -rf "$appdir"
DESTDIR="$appdir" cmake --install "$build_dir" >/dev/null
"$app_dir/packaging/check-bundle.sh" "$appdir"

((appimage)) || exit 0
echo "== AppImage (linuxdeploy + gtk plugin)"
tools="$build_dir/linuxdeploy"
mkdir -p "$tools"
fetch() { [[ -x $tools/$1 ]] || { curl -fsSL -o "$tools/$1" "$2" && chmod +x "$tools/$1"; }; }
fetch linuxdeploy-x86_64.AppImage https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
fetch linuxdeploy-plugin-gtk.sh https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh
export APPIMAGE_EXTRACT_AND_RUN=1 DEPLOY_GTK_VERSION=3 LINUXDEPLOY_OUTPUT_VERSION
LINUXDEPLOY_OUTPUT_VERSION=$(sed -n 's/^set(RIG_PACKAGE_VERSION "\([^"]*\)".*/\1/p' "$app_dir/packaging/Install.cmake")
export PATH="$tools:$PATH"
excludes=(--exclude-library 'libwebkit2gtk-4.1.so*' --exclude-library 'libjavascriptcoregtk-4.1.so*'
          --exclude-library 'libjack.so*' --exclude-library 'libpipewire-0.3.so*' --exclude-library 'libasound.so*')
# Pass 1: deploy the executable + GTK (the gtk plugin runs its own linuxdeploy, which ignores our excludes) …
"$tools/linuxdeploy-x86_64.AppImage" --appdir "$appdir" \
    --executable "$appdir/usr/bin/headroom" \
    --desktop-file "$appdir/usr/share/applications/headroom.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/256x256/apps/headroom.png" \
    "${excludes[@]}" --plugin gtk
# … so drop WebKitGTK/JSC again (the host's copy must be used together with its own helper processes), and give
# appimagetool a PNG .DirIcon (linuxdeploy picks the scalable SVG).
rm -f "$appdir"/usr/lib/libwebkit2gtk-4.1.so* "$appdir"/usr/lib/libjavascriptcoregtk-4.1.so*
rm -rf "$appdir"/usr/share/doc/libwebkit2gtk-4.1-0 "$appdir"/usr/share/doc/libjavascriptcoregtk-4.1-0
rm -f "$appdir/headroom.svg" "$appdir/.DirIcon"
cp "$appdir/usr/share/icons/hicolor/256x256/apps/headroom.png" "$appdir/headroom.png"
ln -s headroom.png "$appdir/.DirIcon"
# Pass 2: package (no plugin, so the excludes hold).
(cd "$out" && "$tools/linuxdeploy-x86_64.AppImage" --appdir "$appdir" "${excludes[@]}" --output appimage)
"$app_dir/packaging/check-bundle.sh" "$appdir"
ls -1 "$out"/*.AppImage
