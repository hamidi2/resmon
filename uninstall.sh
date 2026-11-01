#!/bin/bash
set -e
cd "$(dirname "$0")"
sudo ninja -C build uninstall 2>/dev/null || \
  sudo rm -f /usr/lib/x86_64-linux-gnu/xfce4/panel/plugins/libresmon.so \
             /usr/share/xfce4/panel/plugins/resmon.desktop
echo "resmon uninstalled."
