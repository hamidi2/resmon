#!/bin/bash
# build + install resmon into the Xfce panel plugin dirs
set -e
cd "$(dirname "$0")"
[ -d build ] || meson setup build
ninja -C build
sudo ninja -C build install
echo "Done. Add 'Resource Monitor' to your panel (or restart it)."
