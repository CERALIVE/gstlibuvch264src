#!/usr/bin/env bash
set -euo pipefail

: "${VERSION:?Set VERSION to the release version}"
: "${ARCH:?Set ARCH to arm64 or amd64}"
case "$ARCH" in
  arm64) triplet=aarch64-linux-gnu ;;
  amd64) triplet=x86_64-linux-gnu ;;
  *) printf 'Unsupported architecture: %s\n' "$ARCH" >&2; exit 1 ;;
esac

payload=${1:-build}
output=${2:-dist}
plugin="$payload/usr/lib/$triplet/gstreamer-1.0/libgstlibuvch264src.so"
uvc="$payload/usr/lib/$triplet/libuvc.so.0"
test -f "$plugin"
test -f "$uvc"
highest=$(readelf --version-info "$plugin" "$uvc" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' | sort -Vu | tail -1)
test -n "$highest"
if dpkg --compare-versions "${highest#GLIBC_}" gt 2.41; then
  printf 'Payload requires %s, beyond Debian Trixie\n' "$highest" >&2
  exit 1
fi
readelf -d "$uvc" | grep -q 'Shared library: \[libjpeg.so.62\]'
mkdir -p "$output"
stage=$(mktemp -d "$output/package.XXXXXX")
trap 'rm -rf "$stage"' EXIT
cp -a "$payload/usr" "$stage/"
mkdir -p "$stage/DEBIAN"
cat > "$stage/DEBIAN/control" <<EOF
Package: gstreamer1.0-libuvcsrc
Version: $VERSION
Architecture: $ARCH
Maintainer: CERALIVE <contact@ceralive.com>
Homepage: https://github.com/CERALIVE/gstlibuvcsrc
Section: video
Priority: optional
Depends: libgstreamer1.0-0 (>= 1.26), libgstreamer-plugins-base1.0-0 (>= 1.26), libusb-1.0-0, libjpeg62-turbo, libc6 (>= 2.41)
Provides: gstreamer1.0-libuvch264src (= $VERSION)
Replaces: gstreamer1.0-libuvch264src
Conflicts: gstreamer1.0-libuvch264src
Description: Portable userspace libuvc GStreamer H.264/H.265 capture source
EOF
dpkg-deb --root-owner-group --build "$stage" "$output/gstreamer1.0-libuvcsrc_${VERSION}_${ARCH}.deb"
