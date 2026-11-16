#!/bin/bash -e

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Builds lichii and makes a self-contained copy of the app, as the release workflow does:
# Qt and the Homebrew libraries go inside the bundle, so it runs on a Mac without them.
# usage: tools/package-macos.sh [build dir (build/release)] [output dir (build/package)]

set -o pipefail

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
    local dependencies line name
    if ! dependencies=$(otool -L "$1"); then
        echo "Failed to inspect dependencies for $1" >&2
        return 1
    fi
    while IFS= read -r line; do
        if [[ "$line" =~ ^[[:space:]]+(@rpath/[^[:space:]]+) ]]; then
            name=${BASH_REMATCH[1]}
            if [ ! -e "$app/Contents/Frameworks/${name#@rpath/}" ]; then
                printf '%s\n' "$name"
            fi
        fi
    done <<< "$dependencies"
}

candidate_file=$(mktemp)
trap 'rm -f "$candidate_file"' EXIT
if ! find "$app/Contents/PlugIns" -name '*.dylib' -print0 > "$candidate_file"; then
    echo "Failed to list app plugins" >&2
    exit 1
fi
while IFS= read -r -d '' plugin; do
    if ! missing=$(unresolved "$plugin"); then
        exit 1
    fi
    if [ -n "$missing" ]; then
        echo "Dropping ${plugin#"$app"/}: needs ${missing//$'\n'/ }"
        rm -f "$plugin"
    fi
done < "$candidate_file"

if ! find "$app/Contents" -type f \( -perm -u+x -o -name '*.dylib' \) -print0 \
    > "$candidate_file"; then
    echo "Failed to list app binaries" >&2
    exit 1
fi
binaries=()
while IFS= read -r -d '' candidate; do
    if ! description=$(file -b "$candidate"); then
        echo "Failed to identify binary type for $candidate" >&2
        exit 1
    fi
    case "$description" in
        *Mach-O*) binaries+=("$candidate") ;;
    esac
done < "$candidate_file"

remove_nonbundle_rpaths() {
    local binary="$1" load_commands rpaths rpath
    if ! load_commands=$(otool -l "$binary"); then
        echo "Failed to inspect rpaths for $binary" >&2
        return 1
    fi
    if ! rpaths=$(awk '/cmd LC_RPATH/ {getline; getline; print $2}' <<< "$load_commands"); then
        echo "Failed to parse rpaths for $binary" >&2
        return 1
    fi
    while IFS= read -r rpath; do
        case "$rpath" in
            /opt/homebrew/* | /usr/local/* | "$PWD"/*)
                install_name_tool -delete_rpath "$rpath" "$binary" 2>/dev/null || return 1
                ;;
        esac
    done <<< "$rpaths"
}

for binary in "${binaries[@]}"; do
    remove_nonbundle_rpaths "$binary"
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

inspect_binary() {
    local binary="$1" dependencies load_commands rpaths rpath missing line first
    if ! dependencies=$(otool -L "$binary"); then
        echo "Failed to inspect linked libraries for $binary" >&2
        return 1
    fi
    first=1
    while IFS= read -r line; do
        if [ "$first" -eq 1 ]; then
            first=0
            continue
        fi
        case "$line" in
            *"/usr/lib/libhvf.dylib"*)
                echo "::error::$binary links /usr/lib/libhvf.dylib, unavailable on macOS 15.0 to 15.3" >&2
                return 1
                ;;
            *"/opt/homebrew/"* | *"/usr/local/"* | *"$PWD/"*)
                echo "Reference outside the app bundle in $binary: $line" >&2
                return 1
                ;;
        esac
    done <<< "$dependencies"
    if ! load_commands=$(otool -l "$binary"); then
        echo "Failed to inspect load commands for $binary" >&2
        return 1
    fi
    if ! rpaths=$(awk '/cmd LC_RPATH/ {getline; getline; print $2}' <<< "$load_commands"); then
        echo "Failed to parse rpaths for $binary" >&2
        return 1
    fi
    while IFS= read -r rpath; do
        case "$rpath" in
            /opt/homebrew/* | /usr/local/* | "$PWD"/*)
                echo "Rpath outside the app bundle in $binary: $rpath" >&2
                return 1
                ;;
        esac
    done <<< "$rpaths"
    if ! missing=$(unresolved "$binary"); then
        return 1
    fi
    if [ -n "$missing" ]; then
        echo "Dependencies missing from the app bundle for $binary: ${missing//$'\n'/ }" >&2
        return 1
    fi
}

for binary in "${binaries[@]}"; do
    inspect_binary "$binary"
done

ditto -c -k --keepParent "$app" "$out/lichii-macos-arm64.zip"
echo "Packaged $app and $out/lichii-macos-arm64.zip"
