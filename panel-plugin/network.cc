/*
 * resmon - network monitor data layer.
 *
 * Based on xfce4-systemload-plugin network.cc (c) 2021-2022 Jan Ziak
 * and xfce4-netload-plugin (display model + Auto_Max algorithm),
 * auto-device logic ported from hamidi2's netload MR !28.
 *
 * Redistribution permitted under the BSD 2-Clause terms of the originals.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "network.h"

/* netload.c constants for the Auto_Max algorithm */
#define INIT_MAX 4096
#define MINIMAL_MAX 1024
#define SHRINK_MAX 0.75

static const char *const PROC_NET_NETSTAT = "/proc/net/netstat";
static const char *const PROC_NET_DEV = "/proc/net/dev";
static const char *const PROC_NET_ROUTE = "/proc/net/route";

/* ------------------------------------------------------------------ */
/* default route (hamidi2 MR !28)                                      */

gchar *
find_default_route_device (void)
{
    FILE *f;
    char line[256];
    char iface[64];
    unsigned long dest, metric, best = G_MAXULONG;
    gchar *best_if = NULL;

    f = fopen (PROC_NET_ROUTE, "r");
    if (f == NULL)
        return NULL;

    if (fgets (line, sizeof (line), f) == NULL)  /* skip header */
    {
        fclose (f);
        return NULL;
    }

    while (fgets (line, sizeof (line), f) != NULL)
    {
        /* Iface Destination Gateway Flags RefCnt Use Metric Mask ... */
        if (sscanf (line, "%63s %lx %*x %*x %*d %*d %lu", iface, &dest, &metric) == 3
            && dest == 0UL && metric < best)
        {
            best = metric;
            g_free (best_if);
            best_if = g_strdup (iface);
        }
    }
    fclose (f);
    return best_if;
}

/* ------------------------------------------------------------------ */
/* per-tick byte counters                                              */

static gint
read_dev_bytes (const gchar *device, guint64 *in, guint64 *out)
{
    FILE *fd;
    char line[512];
    char ifname[64];
    guint64 rin, rout;

    if (device == NULL || device[0] == '\0')
        return -1;

    fd = fopen (PROC_NET_DEV, "r");
    if (fd == NULL)
        return -1;

    /* skip the two header lines */
    if (fgets (line, sizeof (line), fd) == NULL ||
        fgets (line, sizeof (line), fd) == NULL)
    {
        fclose (fd);
        return -1;
    }

    rin = rout = 0;
    while (fgets (line, sizeof (line), fd) != NULL)
    {
        char *colon = strchr (line, ':');
        if (colon == NULL)
            continue;
        *colon = '\0';
        if (sscanf (line, " %63s", ifname) != 1 || strcmp (ifname, device) != 0)
            continue;
        /* bytes packets errs drop fifo frame compressed multicast | ... */
        if (sscanf (colon + 1, "%lu %*u %*u %*u %*u %*u %*u %*u %lu",
                    &rin, &rout) == 2)
        {
            fclose (fd);
            *in = rin;
            *out = rout;
            return 0;
        }
    }

    fclose (fd);
    return -1;
}

/* IpExt InOctets/OutOctets: locate the columns by header names so that
 * kernel-side reordering does not silently corrupt the values. */
static gint
read_ipext_bytes (guint64 *in, guint64 *out)
{
    gchar *buf = NULL;
    char *hdr, *val, *tok, *save;
    int in_col = -1, out_col = -1, col;
    guint64 rin = 0, rout = 0;
    gsize length;
    GError *error = NULL;

    /* the file is larger than 4 KiB on modern kernels (the stock plugin's
     * fixed 4 KiB buffer cuts off IpExt) - read it completely */
    if (!g_file_get_contents (PROC_NET_NETSTAT, &buf, &length, &error))
    {
        g_error_free (error);
        return -1;
    }

    hdr = strstr (buf, "IpExt:");
    if (hdr == NULL)
    {
        g_free (buf);
        return -1;
    }
    val = strchr (hdr, '\n');
    if (val == NULL)
    {
        g_free (buf);
        return -1;
    }
    *val = '\0';  /* terminate the header line */
    val++;
    {
        char *val_end = strchr (val, '\n');
        if (val_end != NULL)
            *val_end = '\0';  /* keep the value line self-contained */
    }

    /* header line: map column names (the leading "IpExt:" label counts as a
     * column in both lines, keeping the indices consistent) */
    col = 0;
    for (tok = strtok_r (hdr, " \t\n", &save); tok != NULL; tok = strtok_r (NULL, " \t\n", &save))
    {
        if (strcmp (tok, "InOctets") == 0)
            in_col = col;
        else if (strcmp (tok, "OutOctets") == 0)
            out_col = col;
        col++;
    }
    if (in_col < 0 || out_col < 0)
        return -1;

    /* value line: same token order */
    col = 0;
    for (tok = strtok_r (val, " \t\n", &save); tok != NULL; tok = strtok_r (NULL, " \t\n", &save))
    {
        if (col == in_col)
            rin = g_ascii_strtoull (tok, NULL, 10);
        else if (col == out_col)
            rout = g_ascii_strtoull (tok, NULL, 10);
        col++;
    }
    if (col <= in_col || col <= out_col)
    {
        g_free (buf);
        return -1;
    }

    *in = rin;
    *out = rout;
    g_free (buf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* rate calculation + Auto_Max                                         */

gint
read_net_rates (NetSource source, const gchar *device, NetData *net)
{
    static guint64 bytes[2];
    static guint64 last_time;
    static gchar last_iface[64] = "";
    static gboolean have_prev = FALSE;

    guint64 in = 0, out = 0;
    gchar *auto_dev = NULL;
    const gchar *used_dev = NULL;
    guint64 now;
    gint retval = -1;

    g_return_val_if_fail (net != NULL, -1);

    now = (guint64) g_get_monotonic_time ();

    if (source == NET_SOURCE_SUM)
    {
        retval = read_ipext_bytes (&in, &out);
        if (retval == 0)
            g_snprintf (net->iface, sizeof (net->iface), "%s", "all");
    }
    else
    {
        if (source == NET_SOURCE_AUTO)
        {
            auto_dev = find_default_route_device ();
            used_dev = auto_dev;
        }
        else
            used_dev = device;

        if (used_dev != NULL && used_dev[0] != '\0')
            retval = read_dev_bytes (used_dev, &in, &out);
        if (retval == 0)
            g_snprintf (net->iface, sizeof (net->iface), "%s", used_dev);

        g_free (auto_dev);
    }

    if (retval != 0)
    {
        /* keep the previous scale; report zero rates */
        net->in_bps = 0;
        net->out_bps = 0;
        return -1;
    }

    /* counter reset (ifdown etc.) invalidates the previous sample */
    if (!have_prev
        || now <= last_time
        || in < bytes[0] || out < bytes[0 + 1]
        || strcmp (last_iface, net->iface) != 0)
    {
        bytes[0] = in;
        bytes[1] = out;
        last_time = now;
        g_snprintf (last_iface, sizeof (last_iface), "%s", net->iface);
        have_prev = TRUE;
        net->in_bps = 0;
        net->out_bps = 0;
        return 0;
    }

    {
        gdouble dt = (gdouble) (now - last_time) / 1e6;
        gulong rin_bps = (gulong) ((in - bytes[0]) / dt);
        gulong rout_bps = (gulong) ((out - bytes[1]) / dt);

        /* Auto_Max: grow instantly, shrink slowly (netload algorithm) */
        if (rin_bps > net->max_in)
            net->max_in = rin_bps;
        else if ((gdouble) rin_bps < (gdouble) net->max_in * SHRINK_MAX
                 && net->max_in > MINIMAL_MAX)
            net->max_in = (gulong) ((gdouble) net->max_in * SHRINK_MAX);

        if (rout_bps > net->max_out)
            net->max_out = rout_bps;
        else if ((gdouble) rout_bps < (gdouble) net->max_out * SHRINK_MAX
                 && net->max_out > MINIMAL_MAX)
            net->max_out = (gulong) ((gdouble) net->max_out * SHRINK_MAX);

        net->in_bps = rin_bps;
        net->out_bps = rout_bps;
    }

    bytes[0] = in;
    bytes[1] = out;
    last_time = now;
    g_snprintf (last_iface, sizeof (last_iface), "%s", net->iface);

    return 0;
}

/* ------------------------------------------------------------------ */
/* human readable rates (netload format_byte_humanreadable, bytes/s)   */

gchar *
format_rate_humanreadable (gulong bytes_per_sec, int digits)
{
    static const char *const unit_names_ps[] = { "B/s", "KiB/s", "MiB/s", "GiB/s", "TiB/s" };
    unsigned int uidx = 1;
    double number = bytes_per_sec / 1024.0;
    char fmt[16], buf[64];

    if (digits < 0 || digits >= 10)
        digits = 2;

    while (number >= 1024.0 && uidx < G_N_ELEMENTS (unit_names_ps) - 1)
    {
        number /= 1024.0;
        uidx++;
    }

    g_snprintf (fmt, sizeof (fmt), "%%.%df %%s", digits);
    g_snprintf (buf, sizeof (buf), fmt, number, unit_names_ps[uidx]);
    return g_strdup (buf);
}
