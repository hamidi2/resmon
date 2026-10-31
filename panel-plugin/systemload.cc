/*
 * This file is part of Xfce (https://gitlab.xfce.org).
 *
 * Copyright (c) 2003 Riccardo Persichetti <riccardo.persichetti@tin.it>
 * Copyright (c) 2010 Florian Rivoal <frivoal@xfce.org>
 * Copyright (c) 2012 David Schneider <dnschneid@gmail.com>
 * Copyright (c) 2014-2017 Landry Breuil <landry@xfce.org>
 * Copyright (c) 2021 Simon Steinbeiss <simon@xfce.org>
 * Copyright (c) 2022 Jan Ziak <0xe2.0x9a.0x9b@xfce.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>

#include <libxfce4util/libxfce4util.h>
#include <libxfce4ui/libxfce4ui.h>
#include <libxfce4panel/libxfce4panel.h>
#include <xfconf/xfconf.h>

#ifdef HAVE_UPOWER_GLIB
#include <upower.h>
#endif

#include "cpu.h"
#include "memswap.h"
#include "network.h"
#include "plugin.h"
#include "settings.h"
#include "uptime.h"



struct t_command {
    bool  enabled;
    gchar *command_text;
};

struct t_monitor {
    GtkWidget  *box;
    GtkWidget  *label;
    GtkWidget  *status;
    GtkWidget  *ebox;

    gulong     value_read; /* Range: 0% ... 100% */
};

/* resmon network section (netload-style: two bars + byte/s value labels) */
struct t_netmon {
    GtkWidget  *ebox;        /* tooltip carrier for the whole section */
    GtkWidget  *box;         /* outer box of the section */
    GtkWidget  *label;       /* optional caption, like the sysload monitors */
    GtkWidget  *bars_box;    /* the two adjacent bars */
    GtkWidget  *status[2];   /* progress bars: 0=in 1=out */
    GtkWidget  *value_label[2];
    GtkCssProvider *css[2];

    NetData     data;
};

static const gchar *const NET_DIR_NAME[2] = { "Incoming", "Outgoing" };


struct t_uptime_monitor {
    GtkWidget  *label;
    GtkWidget  *ebox;

    gulong     value_read;
};

struct t_global_monitor {
    XfcePanelPlugin   *plugin;
    SystemloadConfig  *config;
    GtkWidget         *ebox;
    GtkWidget         *box;     /* systemload monitors + uptime */
    GtkWidget         *outer;   /* sysload box + net section, per placement */
    guint             panel_nrows;
    t_netmon          net;
    guint             timeout, timeout_seconds;
    bool              use_timeout_seconds;
    guint             timeout_id;
    guint             setup_idle_id;
    t_command         command;
    t_monitor         *monitor[4];
    t_uptime_monitor  uptime;
#ifdef HAVE_UPOWER_GLIB
    UpClient          *upower;
#endif
};



static const SystemloadMonitor VISUAL_ORDER[] = {
    CPU_MONITOR,
    MEM_MONITOR,
    SWAP_MONITOR,
    NET_MONITOR,
};

static gboolean setup_monitor_cb(gpointer user_data);
static NetPlacement effective_net_placement (t_global_monitor *global);
static void update_net_layout (t_global_monitor *global);
static gboolean monitor_set_size (XfcePanelPlugin *plugin, int size, t_global_monitor *global);
static GtkWidget *resmon_bar_new (const GdkRGBA *color);
static void resmon_bar_set_fraction (GtkWidget *bar, gdouble fraction);
static void resmon_bar_set_color (GtkWidget *bar, const GdkRGBA *color);



static bool
spawn_system_monitor(GtkWidget *w, t_global_monitor *global)
{
    // Spawn defined command; In-terminal: false, Startup-notify: false
    return xfce_spawn_command_line_on_screen(gdk_screen_get_default(),
                                             global->command.command_text,
                                             FALSE, FALSE, NULL);
}

static gboolean
click_event(GtkWidget *w, GdkEventButton *event, t_global_monitor *global)
{
    if(event->button == 1 && global->command.enabled && *(global->command.command_text))
        return spawn_system_monitor(w, global);
    return FALSE;
}

static void
set_fraction(GtkProgressBar *bar, gdouble fraction)
{
    /*
     * Try to avoid a call to GTK's bar_set_fraction() if the new fraction
     * isn't changing the number of pixels (height or width of the bar) already
     * displayed on the screen.
     *
     * max_alloc is independent from horizontal/vertical orientation of the bar.
     */

    GtkAllocation alloc;
    gtk_widget_get_allocation(GTK_WIDGET(bar), &alloc);

    gint max_alloc = MAX(alloc.width, alloc.height);
    if (max_alloc > 1)
        fraction = round(fraction * max_alloc) / max_alloc;

    if (gtk_progress_bar_get_fraction(bar) != fraction)
        gtk_progress_bar_set_fraction(bar, fraction);
}

static void
set_label_text(GtkLabel *label, const gchar *text)
{
    const gchar *displayed_text = gtk_label_get_text(label);
    if (g_strcmp0(displayed_text, text) != 0)
        gtk_label_set_text(label, text);
}

static void
set_tooltip(GtkWidget *w, const gchar *caption)
{
    gchar *displayed_caption = gtk_widget_get_tooltip_text(w);
    if (g_strcmp0(displayed_caption, caption) != 0)
        gtk_widget_set_tooltip_text(w, caption);
    g_free(displayed_caption);
}

static void
update_monitors(t_global_monitor *global)
{
    const SystemloadConfig *config = global->config;
    gulong MTotal = 0, MUsed = 0, STotal = 0, SUsed = 0;

    for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
        global->monitor[i]->value_read = 0;

    if (systemload_config_get_enabled (config, CPU_MONITOR))
        global->monitor[CPU_MONITOR]->value_read = read_cpuload();
    if (systemload_config_get_enabled (config, MEM_MONITOR) ||
        systemload_config_get_enabled (config, SWAP_MONITOR))
    {
        gulong mem, swap;
        if (read_memswap(&mem, &swap, &MTotal, &MUsed, &STotal, &SUsed) == 0)
        {
            global->monitor[MEM_MONITOR]->value_read = mem;
            global->monitor[SWAP_MONITOR]->value_read = swap;
        }
    }
    if (systemload_config_get_uptime_enabled (config))
        global->uptime.value_read = read_uptime();

    for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
    {
        const auto monitor = (SystemloadMonitor) i;
        if (monitor == NET_MONITOR)
            continue;
        t_monitor *m = global->monitor[monitor];

        if (systemload_config_get_enabled (config, monitor))
        {
            gulong value = MIN(m->value_read, 100);
            resmon_bar_set_fraction(global->monitor[i]->status, value / 100.0);
        }
    }

    if (systemload_config_get_enabled (config, NET_MONITOR))
    {
        t_netmon *net = &global->net;

        if (read_net_rates (systemload_config_get_net_source (config),
                            systemload_config_get_net_device (config),
                            &net->data) == 0)
        {
            gulong max[2] = { net->data.max_in, net->data.max_out };
            gulong rate[2] = { net->data.in_bps, net->data.out_bps };

            for (int d = 0; d < 2; d++)
            {
                gdouble frac = (max[d] > 0) ? (gdouble) rate[d] / (gdouble) max[d] : 0.0;
                if (frac > 1.0) frac = 1.0;
                if (frac < 0.0) frac = 0.0;
                resmon_bar_set_fraction(net->status[d], frac);
            }

            if (systemload_config_get_net_show_values (config))
            {
                gboolean as_bits = systemload_config_get_net_as_bits (config);
                for (int d = 0; d < 2; d++)
                {
                    gchar *text = format_rate_short (rate[d], as_bits);
                    set_label_text(GTK_LABEL(net->value_label[d]), text);
                    g_free (text);
                }
            }

            /* live device name in the sysload row */
            if (systemload_config_get_net_show_label (config))
                set_label_text (GTK_LABEL (net->label),
                                net->data.iface[0] ? net->data.iface : "");

            gchar tooltip[256];
            gchar *in_s = format_rate_humanreadable (rate[0], 2);
            gchar *out_s = format_rate_humanreadable (rate[1], 2);
            g_snprintf (tooltip, sizeof (tooltip), "%s\n%s: %s\n%s: %s",
                        net->data.iface[0] ? net->data.iface : _("network"),
                        _("Incoming"), in_s, _("Outgoing"), out_s);
            g_free (in_s);
            g_free (out_s);
            set_tooltip (net->ebox, tooltip);
        }
    }

    if (systemload_config_get_enabled (config, CPU_MONITOR))
    {
        gchar tooltip[128];
        g_snprintf(tooltip, sizeof(tooltip), _("System Load: %ld%%"), global->monitor[CPU_MONITOR]->value_read);
        set_tooltip(global->monitor[CPU_MONITOR]->ebox, tooltip);
    }

    if (systemload_config_get_enabled (config, MEM_MONITOR))
    {
        gchar tooltip[128];
        g_snprintf(tooltip, sizeof(tooltip), _("Memory: %ldMB of %ldMB used"), MUsed >> 10 , MTotal >> 10);
        set_tooltip(global->monitor[MEM_MONITOR]->ebox, tooltip);
    }

    if (systemload_config_get_enabled (config, SWAP_MONITOR))
    {
        gchar tooltip[128];

        if (STotal)
            g_snprintf(tooltip, sizeof(tooltip), _("Swap: %ldMB of %ldMB used"), SUsed >> 10, STotal >> 10);
        else
            g_snprintf(tooltip, sizeof(tooltip), _("No swap"));

        set_tooltip(global->monitor[SWAP_MONITOR]->ebox, tooltip);
    }

    if (systemload_config_get_uptime_enabled (config))
    {
        gchar days_str[2][32], hours_str[2][32], mins_str[2][32];
        gchar text[128], tooltip[128];

        gint days = global->uptime.value_read / 86400;
        gint hours = (global->uptime.value_read / 3600) % 24;
        gint mins = (global->uptime.value_read / 60) % 60;

        g_snprintf(days_str[0], sizeof(days_str), _("%dd"), days);
        g_snprintf(hours_str[0], sizeof(hours_str), _("%dh"), hours);
        g_snprintf(mins_str[0], sizeof(mins_str), _("%dm"), mins);

        g_snprintf(days_str[1], sizeof(days_str), ngettext("%d day", "%d days", days), days);
        g_snprintf(hours_str[1], sizeof(hours_str), ngettext("%d hour", "%d hours", hours), hours);
        g_snprintf(mins_str[1], sizeof(mins_str), ngettext("%d minute", "%d minutes", mins), mins);

        if (days > 0)
            g_snprintf(text, sizeof(text), "%s %s %s", days_str[0], hours_str[0], mins_str[0]);
        else
            g_snprintf(text, sizeof(text), "%s %s", hours_str[0], mins_str[0]);

        g_snprintf(tooltip, sizeof(tooltip), _("Uptime: %s, %s, %s"), days_str[1], hours_str[1], mins_str[1]);

        set_label_text(GTK_LABEL(global->uptime.label), text);
        set_tooltip(global->uptime.ebox, tooltip);
    }
}

/* Effective placement of the network section. "Below/Above" only makes sense
 * when the panel has multiple rows; with a single row fall back to "right"
 * (per the agreed spec: stacked for a two-row panel, beside for one-row). */
static NetPlacement
effective_net_placement (t_global_monitor *global)
{
    NetPlacement placement = systemload_config_get_net_placement (global->config);

    if ((placement == NET_PLACE_BELOW || placement == NET_PLACE_ABOVE)
        && global->panel_nrows <= 1)
        return NET_PLACE_RIGHT;
    return placement;
}

/* Repack outer = sysload box + net section according to the placement. */
static void
update_net_layout (t_global_monitor *global)
{
    NetPlacement placement = effective_net_placement (global);
    bool vertical_stack = (placement == NET_PLACE_BELOW || placement == NET_PLACE_ABOVE);

    gtk_orientable_set_orientation (GTK_ORIENTABLE (global->outer),
                                    vertical_stack ? GTK_ORIENTATION_VERTICAL
                                                   : GTK_ORIENTATION_HORIZONTAL);
    /* stacked: center BOTH the sysload row and the network content */
    gtk_widget_set_halign (global->box,
                           vertical_stack ? GTK_ALIGN_CENTER : GTK_ALIGN_FILL);
    gtk_widget_set_halign (global->net.box,
                           vertical_stack ? GTK_ALIGN_CENTER : GTK_ALIGN_FILL);

    /* detach both children from any previous parent (hold refs across the
     * remove so the widgets survive the unparenting) */
    g_object_ref (global->box);
    g_object_ref (global->net.ebox);
    gtk_container_remove (GTK_CONTAINER (global->outer), global->box);
    if (gtk_widget_get_parent (global->net.ebox) == global->outer)
        gtk_container_remove (GTK_CONTAINER (global->outer), global->net.ebox);

    bool net_first = (placement == NET_PLACE_ABOVE || placement == NET_PLACE_LEFT);
    GtkWidget *first  = net_first ? global->net.ebox : global->box;
    GtkWidget *second = net_first ? global->box : global->net.ebox;

    gtk_box_pack_start (GTK_BOX (global->outer), first, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (global->outer), second, TRUE, TRUE, 0);
    g_object_unref (global->box);
    g_object_unref (global->net.ebox);
}


/* ---- resmon custom progress bar ------------------------------------
 * A minimal drawing-area based bar. GtkProgressBar carries an 82px minimum
 * height for inverted vertical bars (GTK min-size quirk) that breaks the
 * two-row layout, so we draw the bar ourselves: trough + colored fill. */
#define RESMON_BAR_FRACTION "resmon-bar-fraction"
#define RESMON_BAR_COLOR    "resmon-bar-color"

static void
resmon_bar_draw (GtkWidget *bar, cairo_t *cr)
{
    GtkAllocation alloc;
    gtk_widget_get_allocation (bar, &alloc);
    gdouble frac = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (bar), RESMON_BAR_FRACTION)) / 65535.0;
    GdkRGBA *color = (GdkRGBA*) g_object_get_data (G_OBJECT (bar), RESMON_BAR_COLOR);
    bool vertical = alloc.height >= alloc.width;

    /* trough */
    const GdkRGBA trough_color = { 0.55f, 0.55f, 0.55f, 0.45f };
    gdk_cairo_set_source_rgba (cr, &trough_color);
    cairo_rectangle (cr, 0, 0, alloc.width, alloc.height);
    cairo_fill (cr);

    /* progress */
    if (color != NULL && frac > 0.0)
    {
        gdk_cairo_set_source_rgba (cr, color);
        if (vertical)
        {
            gint fill_h = (gint) (alloc.height * frac + 0.5);
            /* inverted: fill grows bottom-up */
            cairo_rectangle (cr, 0, alloc.height - fill_h, alloc.width, fill_h);
        }
        else
            cairo_rectangle (cr, 0, 0, (gint) (alloc.width * frac + 0.5), alloc.height);
        cairo_fill (cr);
    }
}

static gboolean
resmon_bar_draw_cb (GtkWidget *bar, cairo_t *cr, gpointer user_data)
{
    resmon_bar_draw (bar, cr);
    return TRUE;
}

static GtkWidget *
resmon_bar_new (const GdkRGBA *color)
{
    GtkWidget *bar = gtk_drawing_area_new();
    g_object_set_data (G_OBJECT (bar), RESMON_BAR_FRACTION, GUINT_TO_POINTER (0));
    if (color != NULL)
        g_object_set_data_full (G_OBJECT (bar), RESMON_BAR_COLOR,
                                gdk_rgba_copy (color), (GDestroyNotify) gdk_rgba_free);
    g_signal_connect (bar, "draw", G_CALLBACK (resmon_bar_draw_cb), NULL);
    gtk_widget_show (bar);
    return bar;
}

static void
resmon_bar_set_fraction (GtkWidget *bar, gdouble fraction)
{
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    /* 1/65535 granularity avoids redraws that change no pixels */
    guint quant = (guint) (fraction * 65535.0 + 0.5);
    if (GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (bar), RESMON_BAR_FRACTION)) == quant)
        return;
    g_object_set_data (G_OBJECT (bar), RESMON_BAR_FRACTION, GUINT_TO_POINTER (quant));
    gtk_widget_queue_draw (bar);
}

static void
resmon_bar_set_color (GtkWidget *bar, const GdkRGBA *color)
{
    GdkRGBA *copy = gdk_rgba_copy (color);
    GdkRGBA *old = (GdkRGBA*) g_object_steal_data (G_OBJECT (bar), RESMON_BAR_COLOR);
    if (old != NULL)
        gdk_rgba_free (old);
    g_object_set_data_full (G_OBJECT (bar), RESMON_BAR_COLOR, copy, (GDestroyNotify) gdk_rgba_free);
    gtk_widget_queue_draw (bar);
}

/* ---- end custom bar ------------------------------------------------- */

static void
monitor_update_orientation (XfcePanelPlugin  *plugin,
                            GtkOrientation    panel_orientation,
                            GtkOrientation    orientation,
                            t_global_monitor *global)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(global->box), panel_orientation);
    for(gsize count = 0; count < G_N_ELEMENTS (global->monitor); count++)
    {
        const auto monitor = (SystemloadMonitor) count;
        if (monitor == NET_MONITOR)
            continue;
        gtk_orientable_set_orientation(GTK_ORIENTABLE(global->monitor[count]->box), panel_orientation);
        gtk_label_set_angle(GTK_LABEL(global->monitor[count]->label),
                            (orientation == GTK_ORIENTATION_HORIZONTAL) ? 0 : -90);
    }
    gtk_label_set_angle(GTK_LABEL(global->uptime.label),
                        (orientation == GTK_ORIENTATION_HORIZONTAL) ? 0 : -90);

    /* network bars: vertical in a horizontal panel (grow bottom-up), like netload */
    {
        t_netmon *net = &global->net;
        for (int d = 0; d < 2; d++)
        {
            gtk_label_set_angle (GTK_LABEL (net->value_label[d]),
                                 (orientation == GTK_ORIENTATION_HORIZONTAL) ? 0 : -90);
        }
        gtk_label_set_angle (GTK_LABEL (net->label),
                             (orientation == GTK_ORIENTATION_HORIZONTAL) ? 0 : -90);
        update_net_layout (global);
    }
}

static void
create_monitor (t_global_monitor *global)
{
    const SystemloadConfig *config = global->config;

    global->box = gtk_box_new(xfce_panel_plugin_get_orientation(global->plugin), 0);
    gtk_widget_show(global->box);

    for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
    {
        SystemloadMonitor monitor = VISUAL_ORDER[i];
        t_monitor *m = global->monitor[monitor];

        if (monitor == NET_MONITOR)
            continue;  /* the network section replaces the old net bar */

        m->label = gtk_label_new (systemload_config_get_label (config, monitor));

        m->status = resmon_bar_new (systemload_config_get_color (config, monitor));

        m->box = gtk_box_new(xfce_panel_plugin_get_orientation(global->plugin), 0);

        gtk_box_pack_start(GTK_BOX(m->box), GTK_WIDGET(m->label), FALSE, FALSE, 0);

        m->ebox = gtk_event_box_new();
        gtk_widget_show(m->ebox);
        gtk_container_add(GTK_CONTAINER(m->ebox), GTK_WIDGET(m->box));

        gtk_event_box_set_visible_window(GTK_EVENT_BOX(m->ebox), FALSE);
        gtk_event_box_set_above_child(GTK_EVENT_BOX(m->ebox), TRUE);

        gtk_widget_show(GTK_WIDGET(m->status));

        gtk_box_pack_start(GTK_BOX(m->box), GTK_WIDGET(m->status), FALSE, FALSE, 0);
        gtk_widget_set_valign(GTK_WIDGET(m->ebox), GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(global->box), GTK_WIDGET(m->ebox), FALSE, FALSE, 0);

        gtk_widget_show_all(GTK_WIDGET(m->ebox));
    }

    global->uptime.ebox = gtk_event_box_new();
    if (systemload_config_get_uptime_enabled (config))
        gtk_widget_show(global->uptime.ebox);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(global->uptime.ebox), FALSE);

    global->uptime.label = gtk_label_new("");

    gtk_widget_show(global->uptime.label);
    gtk_container_add(GTK_CONTAINER(global->uptime.ebox),
                      GTK_WIDGET(global->uptime.label));

    gtk_widget_set_valign(GTK_WIDGET(global->uptime.ebox), GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(global->box),
                       GTK_WIDGET(global->uptime.ebox),
                       FALSE, FALSE, 0);

    /* --- resmon network section --- */
    {
        t_netmon *net = &global->net;
        const GdkRGBA *colors_default[2] = {
            systemload_config_get_net_color_in (config),
            systemload_config_get_net_color_out (config),
        };

        net->ebox = gtk_event_box_new();
        gtk_event_box_set_visible_window(GTK_EVENT_BOX(net->ebox), FALSE);
        gtk_event_box_set_above_child(GTK_EVENT_BOX(net->ebox), TRUE);

        net->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        net->bars_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

        for (int d = 0; d < 2; d++)
        {
            net->status[d] = resmon_bar_new (colors_default[d]);

            net->value_label[d] = gtk_label_new("");

                gtk_box_pack_start(GTK_BOX(net->bars_box), net->status[d], FALSE, FALSE, 0);
            gtk_widget_show(net->status[d]);
        }

        /* netload layout: [label][in value][in bar][out bar][out value] -
         * bars adjacent in the middle, values on the flanks. The value
         * labels are fixed-width monospace so rate changes never move
         * anything (nor the sysload row above, via plugin re-centering). */
        for (int d = 0; d < 2; d++)
        {
            /* panel default font; fixed width keeps the layout steady */
            gtk_label_set_width_chars (GTK_LABEL (net->value_label[d]), 6);
            gtk_widget_set_valign (net->value_label[d], GTK_ALIGN_CENTER);
            gtk_widget_show(net->value_label[d]);
        }
        /* both right-aligned: the decimal point stays in a fixed column
         * ("%.1f U" always ends with digit + unit) */
        gtk_label_set_xalign (GTK_LABEL (net->value_label[0]), 1.0);
        gtk_label_set_xalign (GTK_LABEL (net->value_label[1]), 1.0);

        net->label = gtk_label_new ("");
        gtk_widget_set_valign (net->label, GTK_ALIGN_CENTER);
        /* the device name belongs to the network row itself */
        gtk_box_pack_start(GTK_BOX(net->box), net->label, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(net->box), net->value_label[0], FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(net->box), net->bars_box, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(net->box), net->value_label[1], FALSE, FALSE, 0);
        gtk_widget_show(net->bars_box);
        gtk_widget_show(net->box);
        gtk_container_add(GTK_CONTAINER(net->ebox), net->box);
    }

    /* outer container: sysload box + net section, arranged per placement */
    global->outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_show(global->outer);
    g_object_ref(global->box);
    gtk_container_add(GTK_CONTAINER(global->outer), global->box);
    g_object_unref(global->box);

    /* center the content vertically (and horizontally) inside the panel cell */
    gtk_widget_set_valign(global->outer, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(global->outer, GTK_ALIGN_CENTER);

    gtk_container_add(GTK_CONTAINER(global->ebox), GTK_WIDGET(global->outer));
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(global->ebox), FALSE);
    gtk_widget_show(GTK_WIDGET(global->ebox));

    update_net_layout (global);
    update_monitors (global);
}

static t_global_monitor *
monitor_control_new(XfcePanelPlugin *plugin)
{
    t_global_monitor *global = g_new0 (t_global_monitor, 1);
#ifdef HAVE_UPOWER_GLIB
    global->upower = up_client_new();
#endif
    global->plugin = plugin;

    /* initialize xfconf */
    global->config = systemload_config_new (xfce_panel_plugin_get_property_base (plugin));

    /* number of panel rows decides the default network placement
     * (stacked "below" for a two-row panel, "right" for a single row) */
    {
        XfconfChannel *panel_channel = xfconf_channel_get ("xfce4-panel");
        gint nrows = 1;
        GValue value = G_VALUE_INIT;

        /* the property is stored as uint; xfconf_channel_get_int() refuses
         * a type mismatch, so go through the generic getter */
        g_value_init (&value, G_TYPE_UINT);
        if (xfconf_channel_get_property (panel_channel, "/panels/panel-1/nrows", &value))
            nrows = (gint) g_value_get_uint (&value);
        else
        {
            g_value_unset (&value);
            g_value_init (&value, G_TYPE_INT);
            if (xfconf_channel_get_property (panel_channel, "/panels/panel-1/nrows", &value))
                nrows = g_value_get_int (&value);
        }
        g_value_unset (&value);
        global->panel_nrows = MAX(nrows, 1);

        if (!systemload_config_has_net_placement_key (global->config))
        {
            NetPlacement def = (global->panel_nrows > 1) ? NET_PLACE_BELOW : NET_PLACE_RIGHT;
            g_object_set (G_OBJECT (global->config), "net-placement", (guint) def, NULL);
        }
    }

    global->timeout = systemload_config_get_timeout (global->config);
    if (global->timeout < MIN_TIMEOUT)
        global->timeout = MIN_TIMEOUT;
    global->timeout_seconds = systemload_config_get_timeout_seconds (global->config);
    global->use_timeout_seconds = (global->timeout_seconds > 0);

    global->ebox = gtk_event_box_new();
    gtk_widget_show(global->ebox);

    global->command.command_text = g_strdup (systemload_config_get_system_monitor_command (global->config));
    if (strlen(global->command.command_text) > 0)
        global->command.enabled = true;

    xfce_panel_plugin_add_action_widget (plugin, global->ebox);

    for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
        global->monitor[i] = g_new0 (t_monitor, 1);

    systemload_config_on_change (global->config, setup_monitor_cb, global);

    return global;
}

static void
monitor_free(XfcePanelPlugin *plugin, t_global_monitor *global)
{
#ifdef HAVE_UPOWER_GLIB
    if (global->upower) {
        g_object_unref(global->upower);
        global->upower = NULL;
    }
#endif

    if (global->timeout_id)
        g_source_remove(global->timeout_id);
    if (global->setup_idle_id)
        g_source_remove(global->setup_idle_id);

    g_free(global->command.command_text);

    for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
        g_free (global->monitor[i]);

    g_free(global);
}

static gboolean
update_monitors_cb(gpointer user_data)
{
    auto global = (t_global_monitor*) user_data;

    update_monitors (global);
    return TRUE;
}

static void
setup_timer(t_global_monitor *global)
{
    GtkSettings *settings;
    if (global->timeout_id)
        g_source_remove(global->timeout_id);
#ifdef HAVE_UPOWER_GLIB
    if (global->upower && global->use_timeout_seconds) {
        if (up_client_get_on_battery(global->upower)) {
            if (!up_client_get_lid_is_closed(global->upower)) {
                global->timeout_id = g_timeout_add_seconds(
                                        global->timeout_seconds,
                                        update_monitors_cb, global);
            } else {
                /* Don't do any timeout if the lid is closed on battery */
                global->timeout_id = 0;
            }
            return;
        }
    }
#endif
    global->timeout_id = g_timeout_add(global->timeout, update_monitors_cb, global);
    /* reduce the default tooltip timeout to be smaller than the update interval otherwise
     * we won't see tooltips on GTK 2.16 or newer */
    settings = gtk_settings_get_default();
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(settings), "gtk-tooltip-timeout"))
        g_object_set(settings, "gtk-tooltip-timeout",
                     global->timeout - 10, NULL);

}

static void
set_margin (const t_global_monitor *global, GtkWidget *w, gint margin)
{
    if (xfce_panel_plugin_get_orientation (global->plugin) == GTK_ORIENTATION_HORIZONTAL)
    {
        gtk_widget_set_margin_start (w, margin);
        gtk_widget_set_margin_top (w, 0);
    }
    else
    {
        gtk_widget_set_margin_start (w, 0);
        gtk_widget_set_margin_top (w, margin);
    }
}

static void
setup_monitors(t_global_monitor *global)
{
    const SystemloadConfig *config = global->config;

    gtk_widget_hide(GTK_WIDGET(global->uptime.ebox));

    /* determine the number of enabled monitors and the number of enabled labels */
    guint n_enabled = 0, n_enabled_labels = 0;
    for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
    {
        auto monitor = (SystemloadMonitor) i;
        if (monitor == NET_MONITOR)
            continue;  /* network section handled below */
        if (systemload_config_get_enabled (config, monitor))
        {
            bool label_visible = systemload_config_get_use_label (config, monitor) &&
                                 strlen (systemload_config_get_label (config, monitor)) != 0;
            n_enabled++;
            n_enabled_labels += (label_visible ? 1 : 0);
        }
    }

    for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
    {
        const auto monitor = (SystemloadMonitor) i;
        const t_monitor *m = global->monitor[monitor];
        const GdkRGBA *color = NULL;

        if (monitor == NET_MONITOR)
            continue;  /* no widget for the legacy net bar */

        gtk_widget_hide(m->ebox);
        gtk_widget_hide(m->label);
        gtk_label_set_text(GTK_LABEL(m->label), systemload_config_get_label (config, monitor));

        color = systemload_config_get_color (config, monitor);
        if (G_LIKELY (color != NULL))
            resmon_bar_set_color (m->status, color);

        if (systemload_config_get_enabled (config, monitor))
        {
            bool label_visible = systemload_config_get_use_label (config, monitor) &&
                                 strlen (systemload_config_get_label (config, monitor)) != 0;

            gtk_widget_show_all(GTK_WIDGET(m->ebox));
            gtk_widget_set_visible (m->label, label_visible);
        }
    }

    if (systemload_config_get_uptime_enabled (config))
    {
        gtk_widget_show_all (global->uptime.ebox);
    }

    /* resmon network section: visibility + colors + layout */
    if (systemload_config_get_enabled (config, NET_MONITOR))
    {
        t_netmon *net = &global->net;
        const GdkRGBA *colors[2] = {
            systemload_config_get_net_color_in (config),
            systemload_config_get_net_color_out (config),
        };
        bool net_label_visible = systemload_config_get_net_show_label (config);

        for (int d = 0; d < 2; d++)
        {
            if (colors[d] != NULL)
                resmon_bar_set_color (net->status[d], colors[d]);
            gtk_widget_set_visible (net->value_label[d],
                                    systemload_config_get_net_show_values (config));
        }

        gtk_widget_set_visible (net->label, net_label_visible);

        /* text<->bar gap: fixed, user-settable ("Text spacing") */
        gtk_box_set_spacing (GTK_BOX (net->box),
                             (gint) systemload_config_get_text_gap (config));

        /* lock the value labels to the widest possible text so the applet
         * width (and the centered sysload row above) never moves */
        {
            const gchar *samples[] = { "888.8 K", "888.8 Kb", NULL };
            gint wmax = 0;
            for (const gchar **t = samples; *t != NULL; t++)
            {
                PangoLayout *pl = gtk_widget_create_pango_layout (net->value_label[0], *t);
                gint w = 0;
                pango_layout_get_pixel_size (pl, &w, NULL);
                wmax = MAX (wmax, w);
                g_object_unref (pl);
            }
            for (int d = 0; d < 2; d++)
                gtk_widget_set_size_request (net->value_label[d], wmax, -1);
        }

        gtk_widget_show (net->ebox);
    }
    else
        gtk_widget_hide (global->net.ebox);

    update_net_layout (global);

    /* sysload row gaps via explicit margins (box spacing cannot give the
     * uptime boundary its own value): Bar spacing between adjacent visible
     * monitors, Text spacing between the last bar and uptime - and zero
     * trailing margin when uptime is hidden */
    {
        gint bar_gap = (gint) systemload_config_get_net_bar_spacing (global->config);
        gint text_gap = (gint) systemload_config_get_text_gap (global->config);
        gint last_visible = -1;
        for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
        {
            if ((SystemloadMonitor) i == NET_MONITOR)
                continue;
            if (gtk_widget_get_visible (global->monitor[i]->ebox))
                last_visible = (gint) i;
        }
        if (systemload_config_get_uptime_enabled (global->config)
            && gtk_widget_get_visible (global->uptime.ebox))
            gtk_widget_set_margin_start (global->uptime.ebox, text_gap);
        else
            gtk_widget_set_margin_start (global->uptime.ebox, 0);
        gtk_widget_set_margin_end (global->uptime.ebox, 0);

        for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
        {
            if ((SystemloadMonitor) i == NET_MONITOR)
                continue;
            gboolean is_last = ((gint) i == last_visible);
            gtk_widget_set_margin_end (global->monitor[i]->ebox,
                                       is_last ? 0 : bar_gap);
            gtk_widget_set_margin_start (global->monitor[i]->ebox, 0);
        }
    }

    setup_timer (global);
}

static gboolean
setup_monitor_idle_cb (gpointer user_data)
{
    auto global = (t_global_monitor*) user_data;
    global->setup_idle_id = 0;
    monitor_set_size (global->plugin, xfce_panel_plugin_get_size (global->plugin), global);
    setup_monitors (global);
    update_monitors (global);
    return G_SOURCE_REMOVE;
}

/* configuration-changed can arrive inside a GTK signal emission (dialog
 * bindings) - mutating the widget tree there crashes; defer to idle */
static gboolean
setup_monitor_cb(gpointer user_data)
{
    auto global = (t_global_monitor*) user_data;
    if (global->setup_idle_id == 0)
        global->setup_idle_id = g_idle_add (setup_monitor_idle_cb, global);
    return TRUE;
}

static gboolean
monitor_set_size(XfcePanelPlugin *plugin, int size, t_global_monitor *global)
{
    gtk_container_set_border_width (GTK_CONTAINER (global->ebox), (size > 26 ? 2 : 1));
    for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
    {
        if ((SystemloadMonitor) i == NET_MONITOR)
            continue;
        if (xfce_panel_plugin_get_orientation (plugin) == GTK_ORIENTATION_HORIZONTAL)
        {
            gtk_widget_set_size_request(GTK_WIDGET(global->monitor[i]->status), 8, -1);
        }
        else
        {
            gtk_widget_set_size_request(GTK_WIDGET(global->monitor[i]->status), -1, 8);
        }
    }

    /* ONE sizing system for every bar (sysload + network): the "Bar width"
     * setting controls the thickness of all of them */
    {
        bool horizontal_panel = (xfce_panel_plugin_get_orientation (plugin) == GTK_ORIENTATION_HORIZONTAL);
        gint bar_w = (gint) systemload_config_get_net_bar_width (global->config);
        for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
        {
            if ((SystemloadMonitor) i == NET_MONITOR)
                continue;
            if (horizontal_panel)
                gtk_widget_set_size_request (global->monitor[i]->status, bar_w, -1);
            else
                gtk_widget_set_size_request (global->monitor[i]->status, -1, bar_w);
        }
        for (int d = 0; d < 2; d++)
        {
            if (horizontal_panel)
                gtk_widget_set_size_request (global->net.status[d], bar_w, -1);
            else
                gtk_widget_set_size_request (global->net.status[d], -1, bar_w);
        }

        /* ONE gap system, exact value from "Bar spacing" (0 allowed):
         * between sysload monitors and between the network bars */
        gint spacing = (gint) systemload_config_get_net_bar_spacing (global->config);
        gtk_box_set_spacing (GTK_BOX (global->net.bars_box), spacing);
    }


    /* height share between the sysload part and the network section:
     * side-by-side = both span the full panel height (netload-like);
     * stacked = split by panel rows (sysload one row, net the rest) */
    {
        guint nrows = MAX(global->panel_nrows, 1);
        /* the panel passes the FULL plugin height (all rows) for non-small
         * plugins; sysload keeps one row, the network section takes the rest */
        int total = size;
        int sys_h, net_h;
        bool stacked;

        switch (effective_net_placement (global))
        {
            case NET_PLACE_BELOW:
            case NET_PLACE_ABOVE:
                stacked = true;
                break;
            default:
                stacked = false;
                break;
        }

        if (stacked)
        {
            sys_h = total / nrows;
            net_h = total - sys_h;
            if (net_h < 8)
            {
                net_h = 8;
                sys_h = total - net_h;
            }
        }
        else
        {
            sys_h = total;
            net_h = total;
        }

        gint bar_w2 = (gint) systemload_config_get_net_bar_width (global->config);
        /* label->bar gap identical to the network row's number->bar gap */
        for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
        {
            if ((SystemloadMonitor) i == NET_MONITOR)
                continue;
            gtk_box_set_spacing (GTK_BOX (global->monitor[i]->box),
                                 (gint) systemload_config_get_text_gap (global->config));
        }
        {
            GtkWidget *up_parent = gtk_widget_get_parent (global->uptime.label);
            (void) up_parent; /* uptime gap is handled with margins in setup_monitors */
        }
        if (xfce_panel_plugin_get_orientation (plugin) == GTK_ORIENTATION_HORIZONTAL)
        {
            gtk_widget_set_size_request (global->ebox, -1, total);
            gtk_widget_set_size_request (global->box, -1, sys_h);
            gtk_widget_set_size_request (global->net.ebox, -1, net_h);
            for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
            {
                if ((SystemloadMonitor) i == NET_MONITOR)
                    continue;
                gtk_widget_set_size_request (global->monitor[i]->status, bar_w2, sys_h - 6);
            }
            for (int d = 0; d < 2; d++)
                gtk_widget_set_size_request (global->net.status[d], bar_w2, net_h - 6);
        }
        else
        {
            gtk_widget_set_size_request (global->ebox, total, -1);
            gtk_widget_set_size_request (global->box, sys_h, -1);
            gtk_widget_set_size_request (global->net.ebox, net_h, -1);
            for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
            {
                if ((SystemloadMonitor) i == NET_MONITOR)
                    continue;
                gtk_widget_set_size_request (global->monitor[i]->status, sys_h - 6, bar_w2);
            }
            for (int d = 0; d < 2; d++)
                gtk_widget_set_size_request (global->net.status[d],
                                             net_h - (systemload_config_get_net_show_values (global->config) ? 18 : 6), 8);
        }
    }

    setup_monitors (global);

    return TRUE;
}


static void
monitor_set_mode (XfcePanelPlugin *plugin, XfcePanelPluginMode mode, t_global_monitor *global)
{
  GtkOrientation panel_orientation = xfce_panel_plugin_get_orientation (plugin);
  GtkOrientation orientation = (mode == XFCE_PANEL_PLUGIN_MODE_VERTICAL) ?
    GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL;

  /* resmon always spans the full panel height (like netload), so the
   * panel must not shrink it to a single row */
  (void) mode;
  xfce_panel_plugin_set_small(XFCE_PANEL_PLUGIN(plugin), FALSE);

  monitor_update_orientation (plugin, panel_orientation, orientation, global);
  monitor_set_size (plugin, xfce_panel_plugin_get_size (plugin), global);
}

#ifdef HAVE_UPOWER_GLIB
static void
#if UP_CHECK_VERSION(0, 99, 0)
upower_changed_cb(UpClient *client, GParamSpec *pspec, t_global_monitor *global)
#else /* UP_CHECK_VERSION < 0.99 */
upower_changed_cb(UpClient *client, t_global_monitor *global)
#endif /* UP_CHECK_VERSION */
{
    setup_timer(global);
}
#endif /* HAVE_UPOWER_GLIB */

static void
command_entry_changed_cb(GtkEntry *entry, t_global_monitor *global)
{
    g_free (global->command.command_text);
    global->command.command_text = g_strdup (gtk_entry_get_text (GTK_ENTRY (entry)));
    global->command.enabled = (strlen (global->command.command_text) != 0);
}

static void
switch_cb(GtkSwitch *check_button, gboolean state, t_global_monitor *global)
{
    gpointer sensitive_widget = g_object_get_data (G_OBJECT (check_button), "sensitive_widget");
    gtk_switch_set_state (check_button, state);
    if (sensitive_widget)
        gtk_revealer_set_reveal_child (GTK_REVEALER (sensitive_widget), state);
}

static void
monitor_dialog_response (GtkWidget *dlg, int response,
                         t_global_monitor *global)
{
    if (response == GTK_RESPONSE_HELP)
    {
        xfce_dialog_show_help (GTK_WINDOW (dlg), PACKAGE_NAME, NULL, NULL);
    }
    else
    {
        gtk_widget_destroy (dlg);
        xfce_panel_plugin_unblock_menu (global->plugin);
    }
}

static void
change_timeout_cb(GtkSpinButton *spin, t_global_monitor *global)
{
    global->timeout = gtk_spin_button_get_value(spin);

    setup_timer(global);
}

#ifdef HAVE_UPOWER_GLIB
static void
change_timeout_seconds_cb(GtkSpinButton *spin, t_global_monitor *global)
{
    global->timeout_seconds = gtk_spin_button_get_value (spin);
    global->use_timeout_seconds = (global->timeout_seconds != 0);
    setup_timer (global);
}
#endif

/* Creates a label, its mnemonic will point to target.
 * Returns the widget. */
static GtkWidget *new_label (GtkGrid *grid, guint row, const gchar *labeltext, GtkWidget *target)
{
    GtkWidget *label = gtk_label_new_with_mnemonic (labeltext);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start (label, 12);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), target);
    gtk_grid_attach(grid, label, 0, row, 1, 1);
    return label;
}

/* Create a new monitor setting  with gtkswitch, and eventually a color button and a checkbox + entry */
static void
new_monitor_setting (t_global_monitor *global,
                     GtkGrid *grid, int position,
                     const gchar *title, bool color,
                     const gchar *setting)
{
    GtkWidget *sw, *label;
    gchar *markup, *setting_name;
    gboolean enabled = TRUE;

    sw = gtk_switch_new();
    gtk_widget_set_halign (sw, GTK_ALIGN_END);
    gtk_widget_set_valign (sw, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top (sw, 12);
    setting_name = g_strconcat (setting, "-enabled", NULL);
    g_object_get (G_OBJECT (global->config), setting_name, &enabled, NULL);
    g_object_bind_property (G_OBJECT (global->config), setting_name,
                            G_OBJECT (sw), "active",
                            GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
    g_signal_connect (GTK_WIDGET(sw), "state-set",
                      G_CALLBACK(switch_cb), global);
    g_free (setting_name);

    markup = g_markup_printf_escaped ("<b>%s</b>", title);
    label = gtk_label_new (markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top (label, 12);
    gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
    g_free (markup);
    gtk_grid_attach(GTK_GRID(grid), label, 0, position, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sw, 1, position, 1, 1);

    if (g_strcmp0 (setting, "uptime") != 0)
    {
        GtkWidget *revealer = gtk_revealer_new ();
        GtkWidget *subgrid = gtk_grid_new ();
        gtk_container_add (GTK_CONTAINER (revealer), subgrid);
        gtk_revealer_set_reveal_child (GTK_REVEALER (revealer), TRUE);
        g_object_set_data (G_OBJECT(sw), "sensitive_widget", revealer);
        gtk_grid_attach(GTK_GRID(grid), revealer, 0, position + 1, 2, 1);
        gtk_grid_set_column_spacing (GTK_GRID(subgrid), 12);
        gtk_grid_set_row_spacing (GTK_GRID(subgrid), 6);

        label = gtk_label_new_with_mnemonic (_("Label:"));
        gtk_widget_set_halign (label, GTK_ALIGN_START);
        gtk_widget_set_valign (label, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_start (label, 12);
        gtk_grid_attach (GTK_GRID(subgrid), label, 0, 0, 1, 1);

        /* Entry for the optional monitor label */
        GtkWidget *entry = gtk_entry_new ();
        gtk_widget_set_hexpand (entry, TRUE);
        gtk_widget_set_tooltip_text (entry, _("Leave empty to disable the label"));
        setting_name = g_strconcat (setting, "-label", NULL);
        g_object_bind_property (G_OBJECT (global->config), setting_name,
                                G_OBJECT (entry), "text",
                                GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
        g_free (setting_name);
        gtk_grid_attach(GTK_GRID(subgrid), entry, 1, 0, 1, 1);

        if (color)
        {
            /* Colorbutton to set the progressbar color */
            GtkWidget *button = gtk_color_button_new ();
            gtk_color_chooser_set_use_alpha (GTK_COLOR_CHOOSER (button), TRUE);

            gtk_label_set_mnemonic_widget (GTK_LABEL (label), button);
            gtk_widget_set_halign(button, GTK_ALIGN_START);
            gtk_widget_set_margin_start (button, 12);
            setting_name = g_strconcat (setting, "-color", NULL);
            g_object_bind_property (G_OBJECT (global->config), setting_name,
                                    G_OBJECT (button), "rgba",
                                    GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
            g_free (setting_name);
            gtk_grid_attach(GTK_GRID(subgrid), button, 2, 0, 1, 1);
        }
    }

    switch_cb (GTK_SWITCH (sw), enabled, global);
}

static void
monitor_create_options(XfcePanelPlugin *plugin, t_global_monitor *global)
{
    SystemloadConfig *config = global->config;
    GtkWidget *label, *entry, *button, *box;

    static const gchar *FRAME_TEXT[] = {
            N_ ("CPU monitor"),
            N_ ("Memory monitor"),
            N_ ("Network monitor"),
            N_ ("Swap monitor"),
            N_ ("Uptime monitor")
    };
    static const gchar *SETTING_TEXT[] = {
            "cpu",
            "memory",
            "network",
            "swap"
    };

    xfce_panel_plugin_block_menu (plugin);

    GtkWidget *dlg;
    dlg = xfce_titled_dialog_new_with_mixed_buttons (_("Resource Monitor"),
                     GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (plugin))),
                                               GTK_DIALOG_DESTROY_WITH_PARENT,
                                               "window-close-symbolic", _("_Close"), GTK_RESPONSE_OK,
                                               "help-browser", _("_Help"), GTK_RESPONSE_HELP,
                                               NULL);

    g_signal_connect (G_OBJECT (dlg), "response",
                      G_CALLBACK (monitor_dialog_response), global);

    gtk_window_set_position (GTK_WINDOW (dlg), GTK_WIN_POS_CENTER);
    gtk_window_set_icon_name (GTK_WINDOW (dlg), "org.xfce.panel.systemload");

    GtkBox *content = GTK_BOX(gtk_dialog_get_content_area (GTK_DIALOG(dlg)));

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing (GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing (GTK_GRID(grid), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_box_pack_start (content, grid, TRUE, TRUE, 0);

    label = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (label), _("<b>General</b>"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);

    /* Update interval */
    button = gtk_spin_button_new_with_range (MIN_TIMEOUT, MAX_TIMEOUT, 50);
    gtk_label_set_mnemonic_widget (GTK_LABEL(label), button);
    gtk_widget_set_halign (button, GTK_ALIGN_START);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (button), (gfloat)global->timeout);
    g_object_bind_property (G_OBJECT (config), "timeout",
                            G_OBJECT (button), "value",
                            GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
    g_signal_connect (G_OBJECT (button), "value-changed", G_CALLBACK(change_timeout_cb), global);
    box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    label = gtk_label_new ("ms");
    gtk_box_pack_start (GTK_BOX (box), button, FALSE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
    gtk_grid_attach (GTK_GRID (grid), box, 1, 1, 1, 1);
    new_label (GTK_GRID (grid), 1, _("Update interval:"), button);

#ifdef HAVE_UPOWER_GLIB
    /* Power-saving interval */
    button = gtk_spin_button_new_with_range (0, 10, 1);
    gtk_widget_set_halign (button, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(GTK_WIDGET(button), _("Update interval when running on battery (uses regular update interval if set to zero)"));
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (button), (gfloat)global->timeout_seconds);
    g_object_bind_property (G_OBJECT (config), "timeout-seconds",
                            G_OBJECT (button), "value",
                            GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
    g_signal_connect (G_OBJECT (button), "value-changed", G_CALLBACK(change_timeout_seconds_cb), global);
    box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    label = gtk_label_new ("s");
    gtk_box_pack_start (GTK_BOX (box), button, FALSE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
    gtk_grid_attach (GTK_GRID (grid), box, 1, 2, 1, 1);
    new_label (GTK_GRID (grid), 2, _("Power-saving interval:"), button);
#endif

    /* System Monitor */
    entry = gtk_entry_new ();
    gtk_widget_set_hexpand (entry, TRUE);
    gtk_entry_set_text (GTK_ENTRY(entry), global->command.command_text);
    gtk_widget_set_tooltip_text(GTK_WIDGET(entry), _("Launched when clicking on the plugin"));
    g_object_bind_property (G_OBJECT (config), "system-monitor-command",
                            G_OBJECT (entry), "text",
                            GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
    g_signal_connect (G_OBJECT(entry), "changed",
                      G_CALLBACK(command_entry_changed_cb), global);
    gtk_grid_attach (GTK_GRID (grid), entry, 1, 3, 1, 1);
    label = new_label (GTK_GRID (grid), 3, _("System monitor:"), entry);

    /* Add options for the monitors (the network monitor has its own
     * section below; uptime follows the other monitors) */
    {
        guint row = 4;
        for(gsize i = 0; i < G_N_ELEMENTS (global->monitor); i++)
        {
            const SystemloadMonitor monitor = VISUAL_ORDER[i];
            if (monitor == NET_MONITOR)
                continue;
            new_monitor_setting (global, GTK_GRID(grid), row,
                                 _(FRAME_TEXT[monitor]),
                                 true,
                                 SETTING_TEXT[monitor]);
            row += 2;
        }

        /* Uptime monitor options */
        new_monitor_setting (global, GTK_GRID(grid), row,
                             _(FRAME_TEXT[4]), FALSE, "uptime");
        row += 2;

    /* resmon network section options */
    {
        GtkWidget *combo;

        label = gtk_label_new (NULL);
        gtk_label_set_markup (GTK_LABEL (label), _("<b>Network monitor</b>"));
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
        row++;

        /* enable/disable the whole network section */
        {
            GtkWidget *check = gtk_check_button_new_with_label (_("Show network monitor"));
            g_object_bind_property (G_OBJECT (config), "network-enabled",
                                    G_OBJECT (check), "active",
                                    GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
            gtk_grid_attach (GTK_GRID (grid), check, 1, row, 1, 1);
            row++;
        }

        /* traffic source */
        combo = gtk_combo_box_text_new ();
        gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), _("Device (manual)"));
        gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), _("Default route (auto)"));
        gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), _("Sum of all devices"));
        guint source = 0;
        g_object_get (G_OBJECT (config), "net-source", &source, NULL);
        gtk_combo_box_set_active (GTK_COMBO_BOX (combo), source);
        g_signal_connect (combo, "changed",
                          G_CALLBACK (+[](GtkComboBox *box, gpointer data) {
                              gint active = gtk_combo_box_get_active (box);
                              g_object_set (G_OBJECT (data), "net-source", (guint) active, NULL);
                          }), config);
        gtk_grid_attach (GTK_GRID (grid), gtk_label_new (_("Traffic source:")), 0, row, 1, 1);
        gtk_grid_attach (GTK_GRID (grid), combo, 1, row, 1, 1);
        row++;

        /* device: combo listing the available interfaces, free text allowed */
        combo = gtk_combo_box_text_new_with_entry ();
        {
            GDir *dir = g_dir_open ("/sys/class/net", 0, NULL);
            if (dir != NULL)
            {
                const gchar *name;
                while ((name = g_dir_read_name (dir)) != NULL)
                    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), name);
                g_dir_close (dir);
            }
            GtkWidget *child = gtk_bin_get_child (GTK_BIN (combo));
            g_object_bind_property (G_OBJECT (config), "net-device",
                                    G_OBJECT (child), "text",
                                    GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
        }
        gtk_widget_set_tooltip_text (combo, _("Network interface used in manual mode"));
        gtk_grid_attach (GTK_GRID (grid), gtk_label_new (_("Device:")), 0, row, 1, 1);
        gtk_grid_attach (GTK_GRID (grid), combo, 1, row, 1, 1);
        row++;

        /* the device name is taken from the active interface; toggle only */
        {
            GtkWidget *check = gtk_check_button_new_with_label (_("Show device name"));
            g_object_bind_property (G_OBJECT (config), "net-show-label",
                                    G_OBJECT (check), "active",
                                    GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
            gtk_grid_attach (GTK_GRID (grid), check, 1, row, 1, 1);
            row++;
        }

        /* show rate values */
        {
            GtkWidget *check = gtk_check_button_new_with_label (_("Show values"));
            g_object_bind_property (G_OBJECT (config), "net-show-values",
                                    G_OBJECT (check), "active",
                                    GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
            gtk_grid_attach (GTK_GRID (grid), check, 1, row, 1, 1);
            row++;
        }

        /* values as bits (Kb) instead of bytes (K) */
        {
            GtkWidget *check = gtk_check_button_new_with_label (_("Values as bits"));
            g_object_bind_property (G_OBJECT (config), "net-as-bits",
                                    G_OBJECT (check), "active",
                                    GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
            gtk_grid_attach (GTK_GRID (grid), check, 1, row, 1, 1);
            row++;
        }

        /* bar thickness */
        {
            GtkWidget *spin = gtk_spin_button_new_with_range (4, 24, 1);
            g_object_bind_property (G_OBJECT (config), "net-bar-width",
                                    G_OBJECT (spin), "value",
                                    GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
            gtk_grid_attach (GTK_GRID (grid), gtk_label_new (_("Bar width:")), 0, row, 1, 1);
            gtk_grid_attach (GTK_GRID (grid), spin, 1, row, 1, 1);
            row++;
        }

        /* gap between texts (labels/values) and the bars */
        {
            GtkWidget *spin = gtk_spin_button_new_with_range (0, 16, 1);
            g_object_bind_property (G_OBJECT (config), "text-gap",
                                    G_OBJECT (spin), "value",
                                    GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
            gtk_grid_attach (GTK_GRID (grid), gtk_label_new (_("Text spacing:")), 0, row, 1, 1);
            gtk_grid_attach (GTK_GRID (grid), spin, 1, row, 1, 1);
            row++;
        }

        /* gap between the bars and values */
        {
            GtkWidget *spin = gtk_spin_button_new_with_range (0, 16, 1);
            g_object_bind_property (G_OBJECT (config), "net-bar-spacing",
                                    G_OBJECT (spin), "value",
                                    GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
            gtk_grid_attach (GTK_GRID (grid), gtk_label_new (_("Bar spacing:")), 0, row, 1, 1);
            gtk_grid_attach (GTK_GRID (grid), spin, 1, row, 1, 1);
            row++;
        }

        /* placement */
        combo = gtk_combo_box_text_new ();
        gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), _("Below the monitors"));
        gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), _("Above the monitors"));
        gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), _("Left of the monitors"));
        gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), _("Right of the monitors"));
        guint placement = 0;
        g_object_get (G_OBJECT (config), "net-placement", &placement, NULL);
        gtk_combo_box_set_active (GTK_COMBO_BOX (combo), placement);
        g_signal_connect (combo, "changed",
                          G_CALLBACK (+[](GtkComboBox *box, gpointer data) {
                              gint active = gtk_combo_box_get_active (box);
                              g_object_set (G_OBJECT (data), "net-placement", (guint) active, NULL);
                          }), config);
        gtk_grid_attach (GTK_GRID (grid), gtk_label_new (_("Placement:")), 0, row, 1, 1);
        gtk_grid_attach (GTK_GRID (grid), combo, 1, row, 1, 1);
        row++;

        /* in/out colors */
        GtkWidget *color_in = gtk_color_button_new ();
        GtkWidget *color_out = gtk_color_button_new ();
        gtk_color_chooser_set_use_alpha (GTK_COLOR_CHOOSER (color_in), TRUE);
        gtk_color_chooser_set_use_alpha (GTK_COLOR_CHOOSER (color_out), TRUE);
        g_object_bind_property (G_OBJECT (config), "net-color-in",
                                G_OBJECT (color_in), "rgba",
                                GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
        g_object_bind_property (G_OBJECT (config), "net-color-out",
                                G_OBJECT (color_out), "rgba",
                                GBindingFlags (G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL));
        gtk_grid_attach (GTK_GRID (grid), gtk_label_new (_("Colors (in / out):")), 0, row, 1, 1);
        {
            GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_box_pack_start (GTK_BOX (hbox), color_in, FALSE, FALSE, 0);
            gtk_box_pack_start (GTK_BOX (hbox), color_out, FALSE, FALSE, 0);
            gtk_grid_attach (GTK_GRID (grid), hbox, 1, row, 1, 1);
        }
        row++;
    }
    }

    gtk_widget_show_all (dlg);
}

static void
monitor_show_about(XfcePanelPlugin *plugin, t_global_monitor *global)
{
    /* List of authors (in alphabetical order) */
    const gchar *auth[] = {
      "David Schneider <dnschneid@gmail.com>",
      "Florian Rivoal <frivoal@xfce.org>",
      "Jan Ziak <0xe2.0x9a.0x9b@xfce.org>",
      "Landry Breuil <landry@xfce.org>",
      "Riccardo Persichetti <riccardo.persichetti@tin.it>",
      "Simon Steinbeiß <simon@xfce.org>",
      NULL
    };

    gtk_show_about_dialog (NULL,
      "logo-icon-name", "org.xfce.panel.systemload",
      "license", xfce_get_license_text (XFCE_LICENSE_TEXT_BSD),
      "version", PACKAGE_VERSION,
      "program-name", "resmon",
      "comments", _("System load and network traffic monitor\n"
                    "Based on xfce4-systemload-plugin 1.3.2 and xfce4-netload-plugin"),
      "copyright", _("Copyright (c) 2003-2022\n"),
      "authors", auth, NULL);
}

void
resmon_construct (XfcePanelPlugin *plugin)
{
    xfce_textdomain(GETTEXT_PACKAGE, LOCALEDIR, "UTF-8");

    t_global_monitor *global = monitor_control_new (plugin);

    create_monitor (global);
    monitor_set_mode (plugin, xfce_panel_plugin_get_mode (plugin), global);

    setup_monitors (global);

    gtk_container_add (GTK_CONTAINER (plugin), global->ebox);

    update_monitors (global);

#ifdef HAVE_UPOWER_GLIB
    if (global->upower) {
#if UP_CHECK_VERSION(0, 99, 0)
        g_signal_connect (global->upower, "notify", G_CALLBACK(upower_changed_cb), global);
#else /* UP_CHECK_VERSION < 0.99 */
        g_signal_connect (global->upower, "changed", G_CALLBACK(upower_changed_cb), global);
#endif /* UP_CHECK_VERSION */
    }
#endif /* HAVE_UPOWER_GLIB */

    g_signal_connect (plugin, "free-data", G_CALLBACK (monitor_free), global);
    g_signal_connect (plugin, "size-changed", G_CALLBACK (monitor_set_size), global);
    g_signal_connect (plugin, "mode-changed", G_CALLBACK (monitor_set_mode), global);
    g_signal_connect (plugin, "button-press-event", G_CALLBACK (click_event), global);

    xfce_panel_plugin_menu_show_configure (plugin);
    g_signal_connect (plugin, "configure-plugin", G_CALLBACK (monitor_create_options), global);

    xfce_panel_plugin_menu_show_about(plugin);
    g_signal_connect (plugin, "about", G_CALLBACK (monitor_show_about), global);
}
