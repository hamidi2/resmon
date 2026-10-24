/*
 * resmon - network monitor data layer.
 *
 * Merges the xfce4-netload-plugin display model (two bars + Auto_Max scale +
 * human readable byte rates) into the systemload plugin.
 *
 * Data sources:
 *   - "sum"    : /proc/net/netstat IpExt InOctets/OutOctets (global, all interfaces)
 *   - "manual" : /proc/net/dev for the configured interface
 *   - "auto"   : /proc/net/dev for the interface carrying the default route
 *                (logic ported from hamidi2's netload MR !28)
 */

#ifndef _RESMON_NETWORK_H_
#define _RESMON_NETWORK_H_

#include <glib.h>

typedef enum {
    NET_SOURCE_MANUAL = 0,
    NET_SOURCE_AUTO,
    NET_SOURCE_SUM,
} NetSource;

typedef struct {
    /* per-tick rates in bytes/s (0 when no data yet) */
    gulong in_bps;
    gulong out_bps;

    /* Auto_Max scale, bytes/s, per direction (netload algorithm) */
    gulong max_in;
    gulong max_out;

    /* resolved interface name, for the tooltip */
    gchar iface[64];
} NetData;

/* Parse once-per-tick counters and update net rates + auto-max scale. */
gint read_net_rates (NetSource source, const gchar *device, NetData *net);

/* netload's format_byte_humanreadable (bytes/s, "KiB/s" style units). */
gchar *format_rate_humanreadable (gulong bytes_per_sec, int digits);

/* compact panel format: "733.2K" (bytes) / "5.9Mb" (bits) */
gchar *format_rate_short (gulong bytes_per_sec, gboolean as_bits);

/* The device of the default route with the lowest metric, or NULL. */
gchar *find_default_route_device (void);

#endif /* _RESMON_NETWORK_H_ */
