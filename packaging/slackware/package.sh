#!/usr/bin/env bash
# Package the native Slackware build using Slackware's own package tools.
set -euo pipefail
build=$(realpath "${1:-build}")
mkdir -p "${2:-dist}"
out=$(realpath "${2:-dist}")
root=$(cd "$(dirname "$0")/../.." && pwd)
version=$(sed -n 's/^set(CPACK_PACKAGE_VERSION "\([^"]*\)")/\1/p' "$build/CPackConfig.cmake")
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
[[ $(uname -m) = x86_64 ]]
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
DESTDIR="$stage" cmake --install "$build" --prefix /usr --strip
# Slackware uses BSD-style init, not systemd. Keep the templates as documentation.
rm -rf "$stage/usr/lib/systemd"
mkdir -p "$stage/install"
cp "$root/packaging/slackware/slack-desc" "$stage/install/"
cp "$root/packaging/slackware/doinst.sh" "$stage/install/"
(cd "$stage" && makepkg -l y -c y "$out/tak-engine-$version-x86_64-1_slack15.0.txz")
