#!/bin/bash
# build resmon_<ver>_amd64.deb (installs the plugin + desktop file, no config)
set -e
cd "$(dirname "$0")"
VER=1.0.0
WORK=$(mktemp -d)
trap 'rm -rf $WORK' EXIT

DIR=$WORK/resmon_$VER-1_amd64/DEBIAN
mkdir -p $DIR \
  $WORK/resmon_$VER-1_amd64/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins \
  $WORK/resmon_$VER-1_amd64/usr/share/xfce4/panel/plugins \
  $WORK/resmon_$VER-1_amd64/usr/share/doc/resmon

cat > $DIR/control <<CTRL
Package: resmon
Version: $VER-1
Section: x11
Priority: optional
Architecture: amd64
Depends: libc6 (>= 2.39), libglib2.0-0t64, libgtk-3-0t64, libxfce4panel-2.0-4 (>= 4.18),
 libxfce4ui-2-0, libxfce4util7, libxfconf-0-3, libcairo2, libpango-1.0-0, libpangocairo-1.0-0,
 libupower-glib3
Suggests: xfce4-panel (>= 4.18)
Maintainer: Hamid Reza <hamidi2@users.noreply.github.com>
Homepage: https://github.com/hamidi2/resmon
Description: Resource Monitor applet for the Xfce panel
 Merges the CPU/memory/swap/uptime monitors of xfce4-systemload-plugin with a
 full netload-style network monitor: two bars (download/upload), adaptive
 Auto_Max scale, byte/bit rates, manual / default-route / sum-of-all traffic
 sources, configurable layout (below/above/left/right) and spacing.
CTRL

cat > $DIR/postinst <<'POST'
#!/bin/bash
echo "resmon installed. Add 'Resource Monitor' via Panel > Add New Items,"
echo "then restart the panel if it does not appear immediately."
POST
chmod 755 $DIR/postinst

ninja -C build || { meson setup build && ninja -C build; }

cp build/panel-plugin/libresmon.so $WORK/resmon_$VER-1_amd64/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins/
cp build/panel-plugin/resmon.desktop $WORK/resmon_$VER-1_amd64/usr/share/xfce4/panel/plugins/
cp README.md LICENSE $WORK/resmon_$VER-1_amd64/usr/share/doc/resmon/
gzip -9 -c COPYING.systemload > $WORK/resmon_$VER-1_amd64/usr/share/doc/resmon/changelog.gz 2>/dev/null || true

# strip the binary
strip --strip-unneeded $WORK/resmon_$VER-1_amd64/usr/lib/x86_64-linux-gnu/xfce4/panel/plugins/libresmon.so

dpkg-deb --root-owner-group --build $WORK/resmon_$VER-1_amd64 resmon_${VER}-1_amd64.deb
echo "---"
dpkg-deb --info resmon_${VER}-1_amd64.deb | head -12
ls -la resmon_${VER}-1_amd64.deb
