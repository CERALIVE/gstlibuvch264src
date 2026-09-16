#!/usr/bin/env bash
set -euo pipefail
deb=${1:?Usage: package-contract.sh package.deb}
test "$(dpkg-deb -f "$deb" Package)" = gstreamer1.0-libuvcsrc
version=$(dpkg-deb -f "$deb" Version)
test "$(dpkg-deb -f "$deb" Provides)" = "gstreamer1.0-libuvch264src (= $version)"
test "$(dpkg-deb -f "$deb" Replaces)" = gstreamer1.0-libuvch264src
test "$(dpkg-deb -f "$deb" Conflicts)" = gstreamer1.0-libuvch264src
dpkg-deb -f "$deb" Depends | grep -q 'libc6 (>= 2.41)'
case "$(dpkg-deb -f "$deb" Architecture)" in
  arm64) triplet=aarch64-linux-gnu ;;
  amd64) triplet=x86_64-linux-gnu ;;
  *) exit 1 ;;
esac
actual=$(dpkg-deb --fsys-tarfile "$deb" | tar -tf - | grep -v '/$' | sort)
expected=$(printf '%s\n' \
  "./usr/lib/$triplet/gstreamer-1.0/libgstlibuvch264src.so" \
  "./usr/lib/$triplet/libuvc.so" \
  "./usr/lib/$triplet/libuvc.so.0" \
  "./usr/lib/$triplet/libuvc.so.0.0.7" | sort)
test "$actual" = "$expected"
printf 'PASS: package identity, compatibility, Trixie dependency and exact payload\n'
