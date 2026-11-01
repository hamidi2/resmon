# resmon — Resource Monitor for the Xfce Panel

> **Status:** under discussion upstream —
> [xfce4-systemload-plugin#39](https://gitlab.xfce.org/panel-plugins/xfce4-systemload-plugin/-/work_items/39)

A single Xfce panel applet that merges the CPU/memory/swap/uptime monitors of
[xfce4-systemload-plugin](https://gitlab.xfce.org/panel-plugins/xfce4-systemload-plugin)
1.3.2 with the network monitor of
[xfce4-netload-plugin](https://gitlab.xfce.org/panel-plugins/xfce4-netload-plugin) —
so one applet replaces both.

![resmon on a two-row panel](docs/screenshot-wide.png)

## Features

- **System monitors**: CPU, memory, swap (bars) and uptime — as in the stock plugin,
  vertically centered in the panel cell.
- **Network monitor** (netload-style):
  - Two adjacent bars (download = orange `rgb(255,79,0)`, upload = yellow
    `rgb(255,229,0)`, both configurable) with byte rates at the flanks
    (`15.0 K | ██ | 4.1 K`).
  - **Auto_Max scale**: the bar scale adapts to traffic (grows instantly, decays
    slowly — the netload algorithm), so bars are never stuck at zero like the
    stock plugin's fixed 100 Mbit scale.
  - Rates shown as `K`/`M`/`G` bytes (or bits with a `b` suffix, toggleable),
    one decimal — the decimal point stays in a fixed column.
  - Three traffic sources: a **manual** device, the **default-route device**
    (switches instantly when the route moves; ported from
    [netload MR !28](https://gitlab.xfce.org/panel-plugins/xfce4-netload-plugin/-/merge_requests/28)),
    or the **sum of all devices** (IpExt counters, read from the whole
    `/proc/net/netstat` file — the stock plugin's 4 KiB buffer truncates it).
  - Optional live device name shown next to the network row.
- **Layout**: place the network section below/above/left/right of the system
  monitors (default: below on multi-row panels, right on single-row). Side by
  side both rows span the full panel height; stacked they split it.
- **Fully tunable spacing**: `Bar width` and `Bar spacing` control *every* bar
  and gap, `Text spacing` sets the text↔bar gap; value labels are pixel-locked
  so the applet never jitters or resizes with the numbers.
- All configuration is live via **xfconf** (`xfce4-panel` channel, under
  `/plugins/<id>/network/…`).

## Installation

Requirements: meson, ninja, g++, and the development packages for
gtk+-3.0, libxfce4panel-2.0, libxfce4ui-2, libxfce4util-1.0, libxfconf-0
(optionally upower-glib for the power-saving interval).
On Xubuntu/Debian/Ubuntu:

    sudo apt install meson ninja-build g++ libgtk-3-dev libxfce4panel-2.0-dev \
         libxfce4ui-2-dev libxfce4util-dev libxfconf-0-dev libupower-glib-dev

Build and install:

    meson setup build
    ninja -C build
    sudo ninja -C build install

or simply:

    ./install.sh

Then add "Resource Monitor" to the panel (right-click the panel → Panel →
Add New Items…), restart the panel if it doesn't appear immediately.

Uninstall:

    sudo ninja -C build uninstall        # or ./uninstall.sh

## Configuration (xfconf)

Everything in the Properties dialog maps to these keys
(`<base>` = `/plugins/panel-1/plugin-<id>`):

| Key                         | Values / meaning                                  |
|-----------------------------|---------------------------------------------------|
| `network/enabled`           | show the network section                          |
| `network/source`            | 0 = manual device, 1 = default route, 2 = sum all |
| `network/device`            | interface name for manual mode                    |
| `network/show-label`        | live device name next to the net row              |
| `network/show-values`       | byte/bit rates next to the bars                   |
| `network/as-bits`           | rates in bits (`Kb`) instead of bytes (`K`)       |
| `network/placement`         | 0 below, 1 above, 2 left, 3 right                 |
| `network/bar-width`         | bar thickness in px (4–24, default 8)             |
| `network/bar-spacing`       | gap between bars (px)                             |
| `network/text-gap`          | text↔bar gap (px)                                 |
| `network/color-in/out`      | download/upload bar colors                        |

## Licensing

The systemload-derived code is BSD-2-Clause (© 2003-2022 the Xfce
contributors, see `COPYING.systemload`); the netload-derived code — the
default-route lookup and the Auto_Max/averaging algorithm — is GPL-2-or-later
(© the netload authors). The combined work is therefore distributed as
**GPL-2.0-or-later**.

## Credits

Built on the work of the xfce4-systemload-plugin and xfce4-netload-plugin
authors (Riccardo Persichetti, Florian Rivoal, David Schneider, Landry Breuil,
Simon Steinbeiß, Jan Ziak, and others), including the default-route mode
merged upstream as netload MR !28.

## Becoming an Xfce/Xubuntu package

The tree is meson-based and desktop-file integrated, ready to be proposed
upstream: fork it to gitlab.xfce.org, open an issue/MR against
`panel-plugins`, or start a Debian ITP/Ubuntu package. Translations live in
`po/`.
