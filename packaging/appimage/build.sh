#!/bin/sh
# Builds tak-engine-<version>-<arch>.AppImage from a configured, built Release tree.
#   packaging/appimage/build.sh <build-dir> <out-dir>
# Run from the source root. The binaries link only libc/libm (everything else is
# static), so the AppDir is just the install tree plus AppRun: no bundled libraries.
# appimagetool and the AppImage runtime are pinned by release and SHA-256.
set -eu
build=$1
out=$2

tool_release=1.9.1
runtime_release=20251108
cpu=$(uname -m)
case $cpu in
    x86_64)
        tool_sha=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
        runtime_sha=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d ;;
    aarch64)
        tool_sha=f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158
        runtime_sha=00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444 ;;
    *) echo "no AppImage for $cpu" >&2; exit 1 ;;
esac
version=$(grep -oE 'VERSION [0-9]+\.[0-9]+\.[0-9]+' CMakeLists.txt | head -1 | awk '{print $2}')

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fetch() {  # fetch <url> <sha256> <file>
    curl -fsSL --retry 3 -o "$3" "$1"
    echo "$2  $3" | sha256sum -c -
}
fetch "https://github.com/AppImage/appimagetool/releases/download/$tool_release/appimagetool-$cpu.AppImage" \
      "$tool_sha" "$work/appimagetool"
fetch "https://github.com/AppImage/type2-runtime/releases/download/$runtime_release/runtime-$cpu" \
      "$runtime_sha" "$work/runtime"
chmod +x "$work/appimagetool"

app=$work/AppDir
cmake --install "$build" --prefix "$app/usr"
# The server's systemd unit and its notes only mean something to a system install.
rm -rf "$app/usr/lib/systemd" "$app/usr/share/doc/tak-engine/systemd"
find "$app/usr" -type d -empty -delete
cp packaging/appimage/AppRun "$app/AppRun"
chmod +x "$app/AppRun"
cp "$app/usr/share/applications/tak-client.desktop" "$app/"
cp "$app/usr/share/icons/hicolor/256x256/apps/tak-client.png" "$app/"
ln -s tak-client.png "$app/.DirIcon"

mkdir -p "$out"
image="$out/tak-engine-$version-$cpu.AppImage"
# No FUSE inside build containers: the tool runs extracted.
ARCH=$cpu APPIMAGE_EXTRACT_AND_RUN=1 "$work/appimagetool" --runtime-file "$work/runtime" "$app" "$image"
chmod +x "$image"
echo "built $image"
