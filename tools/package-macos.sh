#!/bin/bash -e

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Builds lichii and makes a self-contained copy of the app, as the release workflow does:
# Qt and the Homebrew libraries go inside the bundle, so it runs on a Mac without them.
# usage: tools/package-macos.sh [build dir (build/release)] [output dir (build/package)]

cd "$(dirname "$0")/.."
build=${1:-build/release}
out=${2:-build/package}
app="$out/lichii.app"

cmake --build "$build"

# The build's own app stays as it is: a later build relinks it against Homebrew.
rm -rf "$app"
mkdir -p "$out"
cp -R "$build/bin/lichii.app" "$app"
cp "$build/bin/eden-cli" "$app/Contents/MacOS/eden-cli"

macdeployqt "$app" -executable="$app/Contents/MacOS/eden-cli" -libpath="$(brew --prefix)/lib" \
    -always-overwrite -verbose=1
# Qt's virtual keyboard input plugin pulls in the QML stack, which the emulator doesn't use.
rm -f "$app/Contents/PlugIns/platforminputcontexts/libqtvirtualkeyboardplugin.dylib"

# With Homebrew's split Qt packages macdeployqt copies plugins whose own frameworks (QtSvg, QtPdf)
# it can't find. The emulator uses none of them: a plugin with a dependency missing from the
# bundle is dropped rather than left to fail on another Mac.
unresolved() {
    otool -L "$1" | awk '/@rpath\//{print $1}' | while read -r name; do
        [ -e "$app/Contents/Frameworks/${name#@rpath/}" ] || echo "$name"
    done
}
find "$app/Contents/PlugIns" -name '*.dylib' | while read -r plugin; do
    if [ -n "$(unresolved "$plugin")" ]; then
        echo "Dropping ${plugin#"$app"/}: needs $(unresolved "$plugin" | tr '\n' ' ')"
        rm -f "$plugin"
    fi
done

binaries=$(find "$app/Contents" -type f \( -perm -u+x -o -name '*.dylib' \) \
    -exec sh -c 'file -b "$1" | grep -q Mach-O' _ {} \; -print)
for binary in $binaries; do
    otool -l "$binary" | awk '/cmd LC_RPATH/ {getline; getline; print $2}' |
        { grep -E -e '^/(opt/homebrew|usr/local)/' -e "^$PWD/" || true; } |
        while read -r rpath; do
            install_name_tool -delete_rpath "$rpath" "$binary" 2> /dev/null
        done
done
# Libraries that load their dependencies through @rpath find them in the bundle.
for program in lichii eden-cli; do
    install_name_tool -add_rpath @executable_path/../Frameworks \
        "$app/Contents/MacOS/$program" 2> /dev/null || true
done

# macdeployqt invalidates the signatures, and the Hypervisor CPU backend needs its entitlement.
codesign --force --deep --sign - "$app"
codesign --force --sign - --entitlements dist/eglantine.entitlements "$app/Contents/MacOS/eden-cli"
codesign --force --sign - --entitlements dist/eglantine.entitlements "$app"
codesign --verify --deep --strict "$app"

leaks=$(for binary in $binaries; do
    otool -L "$binary" | grep -E -e '^\s+/(opt/homebrew|usr/local)/' -e "^\s+$PWD/"
    otool -l "$binary" | awk '/cmd LC_RPATH/ {getline; getline; print $2}' |
        grep -E -e '^/(opt/homebrew|usr/local)/' -e "^$PWD/"
    unresolved "$binary"
done || true)
if [ -n "$leaks" ]; then
    echo "References still pointing outside the bundle, or missing from it:"
    echo "$leaks" | sort -u
    exit 1
fi

ditto -c -k --keepParent "$app" "$out/lichii-macos-arm64.zip"
echo "Packaged $app and $out/lichii-macos-arm64.zip"
