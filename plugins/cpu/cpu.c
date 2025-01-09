/*
 * CPU usage plugin to lxpanel
 *
 * Copyright (C) 2006-2008 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
 *               2006-2008 Jim Huang <jserv.tw@gmail.com>
 *               2009 Marty Jack <martyj19@comcast.net>
 *               2009 Jürgen Hötzel <juergen@archlinux.org>
 *               2012 Rafał Mużyło <galtgendo@gmail.com>
 *               2012-2013 Henry Gebhardt <hsggebhardt@gmail.com>
 *               2013 Marko Rauhamaa <marko@pacujo.net>
 *               2014 Andriy Grytsenko <andrej@rep.kiev.ua>
 *               2015 Rafał Mużyło <galtgendo@gmail.com>
 *
 * This file is a part of LXPanel project.
 *
 * Copyright (C) 2004 by Alexandre Pereira da Silva <alexandre.pereira@poli.usp.br>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */
/*A little bug fixed by Mykola <mykola@2ka.mipt.ru>:) */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <sys/sysinfo.h>
#include <stdlib.h>
#include <glib/gi18n.h>

#include "plugin.h"

#define BORDER_SIZE 2

/* #include "../../dbg.h" */

typedef unsigned long long CPUTick;		/* Value from /proc/stat */
typedef float CPUSample;			/* Saved CPU utilization value as 0.0..1.0 */

struct cpu_stat {
    CPUTick u, n, s, i;				/* User, nice, system, idle */
};

/* Private context for CPU plugin. */
typedef struct {
#if GTK_CHECK_VERSION(3, 0, 0)
    GdkRGBA foreground_color;			/* Foreground color for drawing area */
    GdkRGBA background_color;			/* Background color for drawing area */
#else
    GdkColor foreground_color;			/* Foreground color for drawing area */
    GdkColor background_color;			/* Background color for drawing area */
#endif
    PluginGraph graph;
    guint timer;				/* Timer for periodic update */
    struct cpu_stat previous_cpu_stat;		/* Previous value of cpu_stat */
    gboolean show_percentage;				/* Display usage as a percentage */
    config_setting_t *settings;
} CPUPlugin;

static gboolean cpu_update(CPUPlugin * c);

static void cpu_destructor(gpointer user_data);

/* Periodic timer callback. */
static gboolean cpu_update(CPUPlugin * c)
{
    if (g_source_is_destroyed(g_main_current_source()))
        return FALSE;
        /* Open statistics file and scan out CPU usage. */
        struct cpu_stat cpu;
        char buffer[256];
        FILE *stat = fopen ("/proc/stat", "r");
        if (stat == NULL) return TRUE;
        fgets (buffer, 256, stat);
        fclose (stat);
        if (!strlen (buffer)) return TRUE;
        int fscanf_result = sscanf(buffer, "cpu %llu %llu %llu %llu", &cpu.u, &cpu.n, &cpu.s, &cpu.i);
        if (fscanf_result == 4)
        {
            /* Compute delta from previous statistics. */
            struct cpu_stat cpu_delta;
            cpu_delta.u = cpu.u - c->previous_cpu_stat.u;
            cpu_delta.n = cpu.n - c->previous_cpu_stat.n;
            cpu_delta.s = cpu.s - c->previous_cpu_stat.s;
            cpu_delta.i = cpu.i - c->previous_cpu_stat.i;

            /* Copy current to previous. */
            memcpy(&c->previous_cpu_stat, &cpu, sizeof(struct cpu_stat));

            /* Compute user+nice+system as a fraction of total.
             * Introduce this sample to ring buffer, increment and wrap ring buffer cursor. */
            float cpu_uns = cpu_delta.u + cpu_delta.n + cpu_delta.s;
            cpu_uns /= (cpu_uns + cpu_delta.i);
            if (c->show_percentage) sprintf (buffer, "C:%3.0f", cpu_uns * 100.0);
            else buffer[0] = 0;

            graph_new_point (&(c->graph), cpu_uns, 0, buffer);
        }
    return TRUE;
}

/* Handler for configure_event on drawing area. */
static void cpu_configuration_changed (LXPanel *panel, GtkWidget *p)
{
    CPUPlugin *c = lxpanel_plugin_get_data (p);

    GdkRGBA none = {0, 0, 0, 0};
    graph_reload (&(c->graph), panel_get_safe_icon_size (panel), c->background_color, c->foreground_color, none, none);
}

/* Plugin constructor. */
static GtkWidget *cpu_constructor(LXPanel *panel, config_setting_t *settings)
{
    /* Allocate plugin context and set into Plugin private data pointer. */
    CPUPlugin * c = g_new0(CPUPlugin, 1);
    GtkWidget * p;
    int tmp_int;
    const char *str;

	c->settings = settings;
    if (config_setting_lookup_int(settings, "ShowPercent", &tmp_int))
        c->show_percentage = tmp_int != 0;

#if GTK_CHECK_VERSION(3, 0, 0)
    if (config_setting_lookup_string(settings, "Foreground", &str))
    {
	if (!gdk_rgba_parse (&c->foreground_color, str))
		gdk_rgba_parse(&c->foreground_color, "dark gray");
    } else gdk_rgba_parse(&c->foreground_color, "dark gray");

    if (config_setting_lookup_string(settings, "Background", &str))
    {
	if (!gdk_rgba_parse (&c->background_color, str))
		gdk_rgba_parse(&c->background_color, "light gray");
    } else gdk_rgba_parse(&c->background_color, "light gray");
#else
    if (config_setting_lookup_string(settings, "Foreground", &str))
    {
	if (!gdk_color_parse (str, &c->foreground_color))
		gdk_color_parse("dark gray",  &c->foreground_color);
    } else gdk_color_parse("dark gray",  &c->foreground_color);

    if (config_setting_lookup_string(settings, "Background", &str))
    {
	if (!gdk_color_parse (str, &c->background_color))
		gdk_color_parse("light gray",  &c->background_color);
    } else gdk_color_parse("light gray",  &c->background_color);
#endif

    /* Allocate top level widget and set into Plugin widget pointer. */
    p = gtk_event_box_new();
    gtk_widget_set_has_window(p, FALSE);
    lxpanel_plugin_set_data(p, c, cpu_destructor);

    /* Allocate drawing area as a child of top level widget. */
    gtk_widget_add_events(c->graph.da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                 GDK_BUTTON_MOTION_MASK);
    graph_init (&(c->graph));
    gtk_container_add (GTK_CONTAINER (p), c->graph.da);

    /* Show the widget.  Connect a timer to refresh the statistics. */
    gtk_widget_show(c->graph.da);
    cpu_configuration_changed (panel,p);
    c->timer = g_timeout_add(1500, (GSourceFunc) cpu_update, (gpointer) c);
    return p;
}

/* Plugin destructor. */
static void cpu_destructor(gpointer user_data)
{
    CPUPlugin * c = (CPUPlugin *)user_data;

    /* Disconnect the timer. */
    g_source_remove(c->timer);

    /* Deallocate memory. */
    graph_free (&(c->graph));
    g_free(c);
}

static gboolean cpu_apply_configuration (gpointer user_data)
{
	char colbuf[32];
    GtkWidget * p = user_data;
    CPUPlugin * c = lxpanel_plugin_get_data(p);
    config_group_set_int (c->settings, "ShowPercent", c->show_percentage);
#if GTK_CHECK_VERSION(3, 0, 0)
    sprintf (colbuf, "%s", gdk_rgba_to_string (&c->foreground_color));
#else
    sprintf (colbuf, "%s", gdk_color_to_string (&c->foreground_color));
#endif
    config_group_set_string (c->settings, "Foreground", colbuf);
#if GTK_CHECK_VERSION(3, 0, 0)
    sprintf (colbuf, "%s", gdk_rgba_to_string (&c->background_color));
#else
    sprintf (colbuf, "%s", gdk_color_to_string (&c->background_color));
#endif
    config_group_set_string (c->settings, "Background", colbuf);
    return FALSE;
}

/* Callback when the configuration dialog is to be shown. */
static GtkWidget *cpu_configure(LXPanel *panel, GtkWidget *p)
{
    CPUPlugin * dc = lxpanel_plugin_get_data(p);
    return lxpanel_generic_config_dlg(_("CPU Usage"), panel,
        cpu_apply_configuration, p,
        _("Show usage as percentage"), &dc->show_percentage, CONF_TYPE_BOOL,
        _("Foreground colour"), &dc->foreground_color, CONF_TYPE_COLOR,
        _("Background colour"), &dc->background_color, CONF_TYPE_COLOR,
        NULL);
}

FM_DEFINE_MODULE(lxpanel_gtk, cpu)

/* Plugin descriptor. */
LXPanelPluginInit fm_module_init_lxpanel_gtk = {
    .name = N_("CPU Usage Monitor"),
    .config = cpu_configure,
    .description = N_("Display CPU usage"),
    .new_instance = cpu_constructor,
    .reconfigure = cpu_configuration_changed,
};
