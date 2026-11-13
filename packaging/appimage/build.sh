#!/bin/sh
# SPDX-License-Identifier: MIT
# Builds the presentmon-linux AppImage from an existing build tree.
#   packaging/appimage/build.sh <build dir> [output dir]
# Needs linuxdeploy and linuxdeploy-plugin-qt (their AppImages) in PATH or in $LINUXDEPLOY_DIR, and qmake6.
# The AppImage holds the GUI, the presentmon CLI and the Vulkan layer; see AppRun for how the layer gets out.
# It runs on distributions whose glibc is at least the build machine's: build on the oldest one you support
# (.github/workflows/appimage.yml uses Debian 13).
set -eu
src="$(cd "$(dirname "$0")/../.." && pwd)"
build="$(cd "${1:?build dir}" && pwd)"
out="$(mkdir -p "${2:-$build}" && cd "${2:-$build}" && pwd)"
appdir="$build/AppDir"
version="$(sed -n 's/^project(presentmon_linux VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt")"
[ -n "${LINUXDEPLOY_DIR:-}" ] && PATH="$LINUXDEPLOY_DIR:$PATH"
linuxdeploy="$(command -v linuxdeploy-x86_64.AppImage || command -v linuxdeploy)"

rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/presentmon-linux"
cp "$build/bin/presentmon-gui" "$build/bin/presentmon" "$appdir/usr/bin/"
# Outside usr/lib: linuxdeploy rewrites the libraries there, and this one is copied out to the user's disk.
cp "$build/layer/libVkLayer_presentmon.so" "$appdir/usr/share/presentmon-linux/"
strip --strip-unneeded "$appdir/usr/bin/presentmon-gui" "$appdir/usr/bin/presentmon" \
	"$appdir/usr/share/presentmon-linux/libVkLayer_presentmon.so"
api="$(sed -n 's/.*"api_version": "\([^"]*\)".*/\1/p' "$build/layer/VkLayer_presentmon.json")"
sed "s|@PML_LAYER_API_VERSION@|$api|" "$src/src/layer/VkLayer_presentmon.json.in" \
	> "$appdir/usr/share/presentmon-linux/VkLayer_presentmon.json"
cp "$src/LICENSE" "$src/THIRD_PARTY_NOTICES.md" "$appdir/usr/share/presentmon-linux/"

export QMAKE="${QMAKE:-$(command -v qmake6)}"
# Kirigami's platform plugin of the org.kde.desktop style (colours and fonts of the desktop): loaded by name at
# run time, so no scan finds it.
plugins="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
deps=""
if [ -d "$plugins/kf6/kirigami/platform" ]; then
	mkdir -p "$appdir/usr/plugins/kf6/kirigami"
	cp -r "$plugins/kf6/kirigami/platform" "$appdir/usr/plugins/kf6/kirigami/"
	deps="--deploy-deps-only $appdir/usr/plugins/kf6/kirigami/platform"
fi
export QML_SOURCES_PATHS="$src/src/gui/qml:$src/packaging/appimage/qml-imports"
# Wayland is not found by the plugin's dependency scan (it is loaded at run time), nor is offscreen (tests).
export EXTRA_QT_MODULES="svg;waylandcompositor"
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so;libqoffscreen.so"
export APPIMAGE_EXTRACT_AND_RUN=1          # no FUSE in containers
export LINUXDEPLOY_OUTPUT_VERSION="$version"
export NO_STRIP=1                          # linuxdeploy's bundled strip does not know newer ELF sections
cd "$out"
"$linuxdeploy" --appdir "$appdir" \
	--executable "$appdir/usr/bin/presentmon-gui" --executable "$appdir/usr/bin/presentmon" \
	--desktop-file "$src/packaging/appimage/presentmon-gui.desktop" \
	--icon-file "$src/src/gui/data/presentmon-gui.svg" \
	--custom-apprun "$src/packaging/appimage/AppRun" \
	$deps --plugin qt --output appimage
ls -la "$out"/PresentMon-*.AppImage
