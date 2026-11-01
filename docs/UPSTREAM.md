# Upstream introduction package (ready-to-post texts)

Two submission paths; both texts below are ready to paste.

## Path A — GitLab XFCE (recommended first step)

Account: gitlab.xfce.org/hamidi2 (already authored netload MR !28).

**Option A1 — propose resmon as a new panel plugin**
Post at: https://gitlab.xfce.org/xfce/xfce4-panel/-/issues
(or open an issue on panel-plugins asking maintainers where it belongs)

**Option A2 — propose the network monitor as a systemload enhancement**
Post at: https://gitlab.xfce.org/panel-plugins/xfce4-systemload-plugin/-/issues

---

### Text A (paste as-is)

Title: resmon — a systemload+netload merge; willing to upstream

Hi all,

I merged xfce4-systemload-plugin 1.3.2 and xfce4-netload-plugin into a single
applet, "resmon", and would like to upstream it (or its network part) with your
guidance.

Repo + screenshots: https://github.com/hamidi2/resmon (v1.0.0, meson build)

What it adds over the stock plugins:

- netload's network monitor inside systemload: two adjacent bars
  (download/upload, configurable colors) with byte rates at the flanks,
  Auto_Max adaptive scale, and rates as K/M/G (bytes or bits).
- Three traffic sources: manual device, default-route device (netload MR !28,
  which I authored), or the sum of all devices via IpExt. Note: the stock
  systemload network monitor reads /proc/net/netstat with a 4 KiB buffer, which
  truncates the file on modern kernels — IpExt no longer fits, which is one
  reason the bar always showed ~0 on fast links.
- Layout: network section below/above/left/right of the system monitors;
  on multi-row panels it stacks and splits the height (side-by-side rows span
  the full height, netload-style).
- All spacing/widths configurable (bar width, bar spacing, text-to-bar gap);
  value labels are pixel-locked to the widest case so the applet never
  resizes/jitters with the numbers.
- Everything lives under the plugin's xfconf base and is live-updating.

Licensing: the netload-derived parts (default-route lookup, Auto_Max/averaging)
are GPL-2-or-later, the rest BSD-2-Clause; the combined work is distributed as
GPL-2.0-or-later. Happy to rework per your policy (e.g. contributing the
network section directly into systemload instead of a separate plugin).

I can prepare an MR against whichever tree you prefer (a new panel-plugins
project, or as a feature branch of systemload replacing the current network
monitor). Feedback welcome — thanks!

hamidi

---

## Path B — Xubuntu (discourse + packaging)

Post at: https://discourse.ubuntu.com/c/xubuntu (category: "Xubuntu
Development"), or start with a Debian ITP bug (`reportbug wnpp` → ITP:
resmon) if you want packaging first.

### Text B (paste as-is)

Title: resmon — merged systemload + netload applet, packaging proposal

Hi Xubuntu team,

I built "resmon", an Xfce panel applet that combines the CPU/memory/swap/
uptime monitors of xfce4-systemload-plugin with a full network monitor
(netload-style: two bars, adaptive scale, byte rates, default-route device
tracking). Motivation: on multi-row panels the two stock applets fight for
space, the stock network bar reads a truncated /proc/net/netstat and is
effectively broken on modern kernels, and netload is no longer developed.

Repo (v1.0.0, meson, GPL-2.0-or-later, screenshots):
https://github.com/hamidi2/resmon

Would the team be interested in seeing it packaged (I can maintain the Debian
packaging), and potentially considered as a replacement for
xfce4-systemload-plugin + xfce4-netload-plugin in the seed once mature?
Feedback on what a new applet needs to be seed-eligible (translations
coverage, meson upstreaming to xfce.org, etc.) is very welcome.

Thanks!
