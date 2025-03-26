/*
 * Copyright (C) 2006-2008 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
 *               2006-2008 Jim Huang <jserv.tw@gmail.com>
 *               2008 Fred Chien <fred@lxde.org>
 *               2009-2010 Marty Jack <martyj19@comcast.net>
 *               2014-2016 Andriy Grytsenko <andrej@rep.kiev.ua>
 *
 * This file is a part of LXPanel project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <string.h>
#include <stdlib.h>

#include "misc.h"

#include <glib-object.h>
#include <glib/gi18n.h>
#include <libfm/fm-gtk.h>

//#define DEBUG
#include "private.h"
#include "dbg.h"
#include "gtk-compat.h"

#if GTK_CHECK_VERSION(3, 0, 0)
#include <gtk/gtkx.h>
#endif

static void plugin_class_unref(PluginClass * pc);

GQuark lxpanel_plugin_qinit;
GQuark lxpanel_plugin_qconf;
GQuark lxpanel_plugin_qdata;
GQuark lxpanel_plugin_qsize;
static GHashTable *_all_types = NULL;

/* Dynamic parameter for static (built-in) plugins must be FALSE so we will not try to unload them */
#define REGISTER_STATIC_PLUGIN_CLASS(pc) \
do {\
    extern PluginClass pc;\
    register_plugin_class(&pc, FALSE);\
} while (0)

static inline const LXPanelPluginInit *_find_plugin(const char *name)
{
    return g_hash_table_lookup(_all_types, name);
}

static GtkWidget *_old_plugin_config(LXPanel *panel, GtkWidget *instance)
{
#ifndef G_DISABLE_CHECKS
    const LXPanelPluginInit *init = PLUGIN_CLASS(instance);
#endif
    Plugin * plugin;

    g_return_val_if_fail(init != NULL && init->new_instance == NULL, NULL);
    plugin = lxpanel_plugin_get_data(instance);
    if (plugin->class->config)
        plugin->class->config(plugin, GTK_WINDOW(panel));
    return NULL;
}

static void _old_plugin_reconfigure(LXPanel *panel, GtkWidget *instance)
{
#ifndef G_DISABLE_CHECKS
    const LXPanelPluginInit *init = PLUGIN_CLASS(instance);
#endif
    Plugin * plugin;

    g_return_if_fail(init != NULL && init->new_instance == NULL);
    plugin = lxpanel_plugin_get_data(instance);
    if (plugin->class->panel_configuration_changed)
        plugin->class->panel_configuration_changed(plugin);
}

/* Register a PluginClass. */
static void register_plugin_class(PluginClass * pc, gboolean dynamic)
{
    LXPanelPluginInit *init = g_new0(LXPanelPluginInit, 1);
    init->_reserved1 = pc;
    init->name = pc->name;
    init->description = pc->description;
    if (pc->config)
        init->config = _old_plugin_config;
    if (pc->panel_configuration_changed)
        init->reconfigure = _old_plugin_reconfigure;
    init->one_per_system = pc->one_per_system;
    init->expand_available = pc->expand_available;
    init->expand_default = pc->expand_default;
    pc->dynamic = dynamic;
    g_hash_table_insert(_all_types, g_strdup(pc->type), init);
}

/* Load a dynamic plugin. */
static void plugin_load_dynamic(const char * type, const gchar * path)
{
    PluginClass * pc = NULL;

    /* Load the external module. */
    GModule * m = g_module_open(path, G_MODULE_BIND_LAZY);
    if (m != NULL)
    {
        /* Formulate the name of the expected external variable of type PluginClass. */
        char class_name[128];
        g_snprintf(class_name, sizeof(class_name), "%s_plugin_class", type);

        /* Validate that the external variable is of type PluginClass. */
        gpointer tmpsym;
        if (( ! g_module_symbol(m, class_name, &tmpsym))	/* Ensure symbol is present */
        || ((pc = tmpsym) == NULL)
        || (pc->structure_size != sizeof(PluginClass))		/* Then check versioning information */
        || (pc->structure_version != PLUGINCLASS_VERSION)
        || (strcmp(type, pc->type) != 0))			/* Then and only then access other fields; check name */
        {
            g_module_close(m);
            g_warning("%s.so is not a lxpanel plugin", type);
            return;
        }

        /* Register the newly loaded and valid plugin. */
        pc->gmodule = m;
        register_plugin_class(pc, TRUE);
        pc->count = 1;
    }
}

/* Unreference a dynamic plugin. */
static void plugin_class_unref(PluginClass * pc)
{
    pc->count -= 1;

    /* If the reference count drops to zero, unload the plugin if it is dynamic and has declared itself unloadable. */
    if ((pc->count == 0)
    && (pc->dynamic)
    && ( ! pc->not_unloadable))
    {
        g_module_close(pc->gmodule);
    }
}

/* Loads all available old type plugins. Should be removed in future releases. */
static void plugin_get_available_classes(void)
{
#ifndef DISABLE_PLUGINS_LOADING
    GDir * dir = g_dir_open(PACKAGE_LIB_DIR "/lxpanel/plugins", 0, NULL);
    if (dir != NULL)
    {
        const char * file;
        while ((file = g_dir_read_name(dir)) != NULL)
        {
            if (g_str_has_suffix(file, ".so"))
            {
                char * type = g_strndup(file, strlen(file) - 3);
                if (_find_plugin(type) == NULL)
                {
                    /* If it has not been loaded, do it.  If successful, add it to the result. */
                    char * path = g_build_filename(PACKAGE_LIB_DIR "/lxpanel/plugins", file, NULL );
                    plugin_load_dynamic(type, path);
                    g_free(path);
                }
                g_free(type);
            }
        }
        g_dir_close(dir);
    }
#endif
}

/* Recursively set the background of all widgets on a panel background configuration change. */
void plugin_widget_set_background(GtkWidget * w, LXPanel * panel)
{
    if (w != NULL)
    {
        if (gtk_widget_get_has_window(w))
        {
            Panel *p = panel->priv;

            gtk_widget_set_app_paintable(w, ((p->background) || (p->transparent)));
            if (gtk_widget_get_realized(w))
            {
                GdkWindow *window = gtk_widget_get_window(w);
#if !GTK_CHECK_VERSION(3, 0, 0)
                gdk_window_set_back_pixmap(window, NULL, TRUE);
#endif
                if ((p->background) || (p->transparent))
                    /* Reset background for the child, using background of panel */
                    gdk_window_invalidate_rect(window, NULL, TRUE);
#if !GTK_CHECK_VERSION(3, 0, 0)
                else
                    /* Set background according to the current GTK style. */
                    gtk_style_set_background(gtk_widget_get_style(w), window,
                                             GTK_STATE_NORMAL);
#endif
            }
        }

        /* Special handling to get tray icons redrawn. */
        if (GTK_IS_SOCKET(w))
        {
            gtk_widget_hide(w);
#if !GTK_CHECK_VERSION(3, 0, 0)
            gdk_window_process_all_updates();
#endif
            gtk_widget_show(w);
#if !GTK_CHECK_VERSION(3, 0, 0)
            gdk_window_process_all_updates();
#endif
        }

        /* Recursively process all children of a container. */
        if (GTK_IS_CONTAINER(w))
            gtk_container_foreach(GTK_CONTAINER(w), (GtkCallback) plugin_widget_set_background, panel);
    }
}

/* Handler for "button_press_event" signal with Plugin as parameter.
 * External so can be used from a plugin. */
static gboolean lxpanel_plugin_button_press_event(GtkWidget *plugin, GdkEventButton *event, LXPanel *panel)
{
    if (event->button == 3 && /* right button */
        (event->state & gtk_accelerator_get_default_mod_mask()) == 0) /* no key */
    {
        if (is_wizard ()) return TRUE;
#ifdef ENABLE_NLS
		// this message comes via the plugin, which will have switched to its own text domain, so 
		// we need to switch back here...
		textdomain ( GETTEXT_PACKAGE );
#endif
        GtkMenu* popup = (GtkMenu*)lxpanel_get_plugin_menu(panel, plugin, FALSE);
#if GTK_CHECK_VERSION(3, 0, 0)
        gtk_menu_popup_at_pointer (popup, (GdkEvent *) event);
#else
        gtk_menu_popup(popup, NULL, NULL, NULL, NULL, event->button, event->time);
#endif
        return TRUE;
    }
    return FALSE;
}

/* for old plugins compatibility */
gboolean plugin_button_press_event(GtkWidget *widget, GdkEventButton *event, Plugin *plugin)
{
    return lxpanel_plugin_button_press_event(plugin->pwid, event, PLUGIN_PANEL(plugin->pwid));
}

/* Helper for position-calculation callback for popup menus. */
void lxpanel_plugin_popup_set_position_helper(LXPanel * p, GtkWidget * near, GtkWidget * popup, gint * px, gint * py)
{
    gint x, y;
    GtkAllocation allocation;
    GtkAllocation popup_req;
    GdkScreen *screen = NULL;
#if GTK_CHECK_VERSION(3, 0, 0)
    GdkMonitor *monitor;
#else
    gint monitor;
#endif

    /* Get the allocation of the popup menu. */
    gtk_widget_realize(popup);
    gtk_widget_get_allocation(popup, &popup_req);
    if (gtk_widget_is_toplevel(popup))
    {
        GdkRectangle extents;
        /* FIXME: can we wait somehow for WM drawing decorations? */
#if !GTK_CHECK_VERSION(3, 0, 0)
        gdk_window_process_all_updates();
#endif
        gdk_window_get_frame_extents(gtk_widget_get_window(popup), &extents);
        popup_req.width = extents.width;
        popup_req.height = extents.height;
    }

    /* Get the origin of the requested-near widget in screen coordinates. */
    gtk_widget_get_allocation(near, &allocation);
    gdk_window_get_origin(gtk_widget_get_window(near), &x, &y);
    if (!gtk_widget_get_has_window(near))
    {
        /* For non-window widgets allocation is given within the screen */
        x += allocation.x;
        y += allocation.y;
    }

    /* Dispatch on edge to lay out the popup menu with respect to the button.
     * Also set "push-in" to avoid any case where it might flow off screen. */
    switch (p->priv->edge)
    {
        case EDGE_TOP:          y += allocation.height;         break;
        case EDGE_BOTTOM:       y -= popup_req.height;                break;
        case EDGE_LEFT:         x += allocation.width;          break;
        case EDGE_RIGHT:        x -= popup_req.width;                 break;
    }

    /* Push onscreen. */
    if (gtk_widget_has_screen(near))
        screen = gtk_widget_get_screen(near);
    else
        /* panel as a GtkWindow always has a screen */
        screen = gtk_widget_get_screen(GTK_WIDGET(p));
#if GTK_CHECK_VERSION(3, 0, 0)
    monitor = gdk_display_get_monitor_at_point (gdk_screen_get_display (screen), x, y);
    gdk_monitor_get_workarea (monitor, &allocation);
#else
    monitor = gdk_screen_get_monitor_at_point(screen, x, y);
    gdk_screen_get_monitor_geometry(screen, monitor, &allocation);
#endif
    x = CLAMP(x, allocation.x, allocation.x + allocation.width - popup_req.width);
    y = CLAMP(y, allocation.y, allocation.y + allocation.height - popup_req.height);

    *px = x;
    *py = y;
}

/* for old plugins compatibility -- popup_req is ignored here */
void plugin_popup_set_position_helper(Plugin * p, GtkWidget * near, GtkWidget * popup, GtkRequisition * popup_req, gint * px, gint * py)
{
    lxpanel_plugin_popup_set_position_helper(p->panel->topgwin, near, popup, px, py);
}

/* Adjust the position of a popup window to ensure that it is not hidden by the panel.
 * It is observed that some window managers do not honor the strut that is set on the panel. */
void lxpanel_plugin_adjust_popup_position(GtkWidget * popup, GtkWidget * parent)
{
    gint x, y;

    /* Calculate desired position for the popup. */
    lxpanel_plugin_popup_set_position_helper(PLUGIN_PANEL(parent), parent,
                                             popup, &x, &y);
    /* Move the popup to position. */
    gdk_window_move(gtk_widget_get_window(popup), x, y);
}

/* for old plugins compatibility */
void plugin_adjust_popup_position(GtkWidget * popup, Plugin * plugin)
{
    lxpanel_plugin_adjust_popup_position(popup, plugin->pwid);
}

/* Open a specified path in a file manager. */
static gboolean _open_dir_in_file_manager(GAppLaunchContext* ctx, GList* folder_infos,
                                          gpointer user_data, GError** err)
{
    FmFileInfo *fi = folder_infos->data; /* only first is used */
    GAppInfo *app = g_app_info_get_default_for_type("inode/directory", TRUE);
    GFile *gf;
    gboolean ret;

    if (app == NULL)
    {
        g_set_error_literal(err, G_SHELL_ERROR, G_SHELL_ERROR_EMPTY_STRING,
                            _("No file manager is configured."));
        return FALSE;
    }
    gf = fm_path_to_gfile(fm_file_info_get_path(fi));
    folder_infos = g_list_prepend(NULL, gf);
    ret = fm_app_info_launch(app, folder_infos, ctx, err);
    g_list_free(folder_infos);
    g_object_unref(gf);
    g_object_unref(app);
    return ret;
}

gboolean lxpanel_launch_path(LXPanel *panel, FmPath *path)
{
    return fm_launch_path_simple(NULL, NULL, path, _open_dir_in_file_manager, NULL);
}

void lxpanel_plugin_show_config_dialog(GtkWidget* plugin)
{
    const LXPanelPluginInit *init = PLUGIN_CLASS(plugin);
    LXPanel *panel = PLUGIN_PANEL(plugin);
    GtkWidget *dlg = panel->priv->plugin_pref_dialog;

    if (dlg && g_object_get_data(G_OBJECT(dlg), "generic-config-plugin") == plugin)
        return; /* configuration dialog is already shown for this widget */
    g_return_if_fail(panel != NULL);
    dlg = init->config(panel, plugin);
    if (dlg)
        _panel_show_config_dialog(panel, plugin, dlg);
}

#if GLIB_CHECK_VERSION(2, 32, 0)
static GRecMutex _mutex;
#else
static GStaticRecMutex _mutex = G_STATIC_REC_MUTEX_INIT;
#endif

#ifndef DISABLE_PLUGINS_LOADING
FM_MODULE_DEFINE_TYPE(lxpanel_gtk, LXPanelPluginInit, 1)

static gboolean fm_module_callback_lxpanel_gtk(const char *name, gpointer init, int ver)
{
    /* ignore ver for now, only 1 exists */
    return lxpanel_register_plugin_type(name, init);
}
#endif

static gboolean old_plugins_loaded = FALSE;

void lxpanel_prepare_modules(void)
{
    _all_types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    lxpanel_plugin_qdata = g_quark_from_static_string("LXPanel::plugin-data");
    lxpanel_plugin_qinit = g_quark_from_static_string("LXPanel::plugin-init");
    lxpanel_plugin_qconf = g_quark_from_static_string("LXPanel::plugin-conf");
    lxpanel_plugin_qsize = g_quark_from_static_string("LXPanel::plugin-size");
#ifndef DISABLE_PLUGINS_LOADING
    fm_modules_add_directory(PACKAGE_LIB_DIR "/lxpanel/plugins");
    fm_module_register_lxpanel_gtk();
#endif
}

void lxpanel_unload_modules(void)
{
    GHashTableIter iter;
    gpointer key, val;

    g_hash_table_iter_init(&iter, _all_types);
    while(g_hash_table_iter_next(&iter, &key, &val))
    {
        register const LXPanelPluginInit *init = val;
        if (init->new_instance == NULL) /* old type of plugin */
        {
            plugin_class_unref(init->_reserved1);
            g_free(val);
        }
    }
    g_hash_table_destroy(_all_types);
#ifndef DISABLE_PLUGINS_LOADING
    fm_module_unregister_type("lxpanel_gtk");
#endif
    old_plugins_loaded = FALSE;
}

gboolean lxpanel_register_plugin_type(const char *name, const LXPanelPluginInit *init)
{
    const LXPanelPluginInit *data;

    /* validate it */
    if (init->new_instance == NULL || name == NULL || name[0] == '\0')
        return FALSE;
#if GLIB_CHECK_VERSION(2, 32, 0)
    g_rec_mutex_lock(&_mutex);
#else
    g_static_rec_mutex_lock(&_mutex);
#endif
    /* test if it's registered already */
    data = _find_plugin(name);
    if (data == NULL)
    {
        if (init->init)
            init->init();
        g_hash_table_insert(_all_types, g_strdup(name), (gpointer)init);
    }
#if GLIB_CHECK_VERSION(2, 32, 0)
    g_rec_mutex_unlock(&_mutex);
#else
    g_static_rec_mutex_unlock(&_mutex);
#endif
    return (data == NULL);
}

static void _old_plugin_save_hook(const config_setting_t * setting, FILE * f, gpointer user_data)
{
    Plugin *pl = user_data;
    PluginClass *pc = pl->class;
    if (pc->save)
        pc->save(pl, f);
}

/* This is called right before Plugin instance is destroyed */
static void _old_plugin_destroy(gpointer data)
{
    Plugin *pl = data;

    plugin_class_unref(pl->class);

    /* Free the Plugin structure. */
    g_free(pl);
}

static void _on_old_widget_destroy(GtkWidget *widget, Plugin *pl)
{
    /* Never let run it again. */
    g_signal_handlers_disconnect_by_func(widget, _on_old_widget_destroy, pl);
    /* Run the destructor before destroying the top level widget.
     * This prevents problems with the plugin destroying child widgets. */
    pl->class->destructor(pl);
}

static void on_size_allocate(GtkWidget *widget, GdkRectangle *allocation, LXPanel *p)
{
    GdkRectangle *alloc;

    alloc = g_object_get_qdata(G_OBJECT(widget), lxpanel_plugin_qsize);
    if (alloc->x == allocation->x && alloc->y == allocation->y &&
        alloc->width == allocation->width && alloc->height == allocation->height)
        return; /* not changed */
    *alloc = *allocation;
    /* g_debug("size-allocate on %s", PLUGIN_CLASS(widget)->name); */
    plugin_widget_set_background(widget, p);
//    _panel_queue_update_background(p);
//    _queue_panel_calculate_size(p);
}

GtkWidget *lxpanel_add_plugin(LXPanel *p, const char *name, config_setting_t *cfg, gint at)
{
    const LXPanelPluginInit *init;
    GtkWidget *widget;
    config_setting_t *s, *pconf;
    gint expand, padding = 0, border = 0, i;

    CHECK_MODULES();
    if (!old_plugins_loaded)
        plugin_get_available_classes();
    old_plugins_loaded = TRUE;
    init = _find_plugin(name);
    if (init == NULL)
        return NULL;
    /* prepare widget settings */
    if (!init->expand_available)
        expand = 0;
    else if ((s = config_setting_get_member(cfg, "expand")))
        expand = config_setting_get_int(s);
    else
        expand = init->expand_default;
    s = config_setting_get_member(cfg, "padding");
    if (s)
        padding = config_setting_get_int(s);
    s = config_setting_get_member(cfg, "border");
    /* FIXME: this is useless setting, it should be 0 or panel becomes weird */
    if (s)
        border = config_setting_get_int(s);
    /* prepare config and create it if need */
    s = config_setting_add(cfg, "", PANEL_CONF_TYPE_LIST);
    for (i = 0; (pconf = config_setting_get_elem(s, i)); i++)
        if (strcmp(config_setting_get_name(pconf), "Config") == 0)
            break;
    if (!pconf)
        pconf = config_setting_add(s, "Config", PANEL_CONF_TYPE_GROUP);
    /* If this plugin can only be instantiated once, count the instantiation.
     * This causes the configuration system to avoid displaying the plugin as one that can be added. */
    if (init->new_instance) /* new style of plugin */
    {
        widget = init->new_instance(p, pconf);
        if (widget == NULL)
            return widget;
        /* always connect lxpanel_plugin_button_press_event() */
        g_signal_connect(widget, "button-press-event",
                         G_CALLBACK(lxpanel_plugin_button_press_event), p);
        if (init->button_press_event)
            g_signal_connect(widget, "button-press-event",
                             G_CALLBACK(init->button_press_event), p);
    }
    else
    {
        Plugin *pl = g_new0(Plugin, 1);
        PluginClass *pc = init->_reserved1;
        char *conf = config_setting_to_string(pconf), *fp;

        pl->class = pc;
        pl->panel = p->priv;
        widget = NULL;
        fp = &conf[9]; /* skip "Config {\n" */
        /* g_debug("created conf: %s",conf); */
    /* Call the constructor.
     * It is responsible for parsing the parameters, and setting "pwid" to the top level widget. */
        if (pc->constructor(pl, &fp))
            widget = pl->pwid;
        g_free(conf);

        if (widget == NULL) /* failed */
        {
            g_free(pl);
            return widget;
        }

        pc->count += 1;
        g_signal_connect(widget, "destroy", G_CALLBACK(_on_old_widget_destroy), pl);
        config_setting_set_save_hook(pconf, _old_plugin_save_hook, pl);
        lxpanel_plugin_set_data(widget, pl, _old_plugin_destroy);
    }
    gtk_widget_set_name(widget, name);
    gtk_box_pack_start(GTK_BOX(p->priv->box), widget, expand, TRUE, padding);
    if (at >= 0)
        gtk_box_reorder_child(GTK_BOX(p->priv->box), widget, at);
    if (GTK_IS_CONTAINER (widget))
    gtk_container_set_border_width(GTK_CONTAINER(widget), border);
    g_signal_connect(widget, "size-allocate", G_CALLBACK(on_size_allocate), p);
    gtk_widget_show(widget);
    g_object_set_qdata(G_OBJECT(widget), lxpanel_plugin_qconf, cfg);
    g_object_set_qdata(G_OBJECT(widget), lxpanel_plugin_qinit, (gpointer)init);
    g_object_set_qdata_full(G_OBJECT(widget), lxpanel_plugin_qsize,
                            g_new0(GdkRectangle, 1), g_free);
    return widget;
}

/* transfer none - note that not all fields are valid there */
GHashTable *lxpanel_get_all_types(void)
{
    return _all_types;
}

/* sets the supplied image widget to the named icon at the current taskbar size */
void lxpanel_plugin_set_taskbar_icon (LXPanel *p, GtkWidget *image, const char *icon)
{
    GdkPixbuf *pixbuf;

    pixbuf = gtk_icon_theme_load_icon (panel_get_icon_theme (p), icon,
        panel_get_safe_icon_size (p), GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    if (pixbuf)
    {
        gtk_image_set_from_pixbuf (GTK_IMAGE (image), pixbuf);
        g_object_unref (pixbuf);
    }
}

void lxpanel_plugin_set_menu_icon (LXPanel *p, GtkWidget *image, const char *icon)
{
    GdkPixbuf *pixbuf = NULL;

    if (icon)
        pixbuf = gtk_icon_theme_load_icon (panel_get_icon_theme (p), icon,
            panel_get_safe_icon_size (p) > 32 ? 24 : 16, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    if (!pixbuf)
    {
        pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, panel_get_safe_icon_size (p) > 32 ? 24 : 16, 
            panel_get_safe_icon_size (p) > 32 ? 24 : 16);
        gdk_pixbuf_fill (pixbuf, 0xffffff00);
    }
    gtk_image_set_from_pixbuf (GTK_IMAGE (image), pixbuf);
    g_object_unref (pixbuf);
}

GtkWidget *lxpanel_plugin_new_menu_item (LXPanel *p, const char *text, int maxlen, const char *iconname)
{
    GtkWidget *item = gtk_menu_item_new ();
    gtk_widget_set_name (item, "panelmenuitem");
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, MENU_ICON_SPACE);
    GtkWidget *label = gtk_label_new (text);
    GtkWidget *icon = gtk_image_new ();
    lxpanel_plugin_set_menu_icon (p, icon, iconname);

    if (maxlen)
    {
        gtk_label_set_max_width_chars (GTK_LABEL (label), maxlen);
        gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
    }

    gtk_container_add (GTK_CONTAINER (item), box);
    gtk_container_add (GTK_CONTAINER (box), icon);
    gtk_container_add (GTK_CONTAINER (box), label);

    return item;
}

void lxpanel_plugin_update_menu_icon (GtkWidget *item, GtkWidget *image)
{
    GtkWidget *box = gtk_bin_get_child (GTK_BIN (item));
    GList *children = gtk_container_get_children (GTK_CONTAINER (box));
    GtkWidget *img = (GtkWidget *) children->data;
    gtk_container_remove (GTK_CONTAINER (box), img);
    gtk_box_pack_start (GTK_BOX (box), image, FALSE, FALSE, 0);
    gtk_box_reorder_child (GTK_BOX (box), image, 0);
}

void lxpanel_plugin_append_menu_icon (GtkWidget *item, GtkWidget *image)
{
    GtkWidget *box = gtk_bin_get_child (GTK_BIN (item));
    gtk_box_pack_end (GTK_BOX (box), image, FALSE, FALSE, 0);
}

const char *lxpanel_plugin_get_menu_label (GtkWidget *item)
{
    if (!GTK_IS_BIN (item)) return "";
    GtkWidget *box = gtk_bin_get_child (GTK_BIN (item));
    if (!box) return "";
    GList *children = gtk_container_get_children (GTK_CONTAINER (box));
    if (!children) return "";
    while (children->data)
    {
        if (GTK_IS_LABEL ((GtkWidget *) children->data))
            return gtk_label_get_text (GTK_LABEL ((GtkWidget *) children->data));
        children = children->next;
    }
    return "";
}

/*----------------------------------------------------------------------------*/
/* Plugin graph */
/*----------------------------------------------------------------------------*/

/* Redraw entire graph */

static void graph_redraw (PluginGraph *graph, char *label)
{
    unsigned int fontsize, drawing_cursor, i;
    GdkPixbuf *pixbuf;

    cairo_t *cr = cairo_create (graph->pixmap);
    cairo_set_line_width (cr, 1.0);

    /* Erase pixmap */
    cairo_rectangle (cr, 0, 0, graph->pixmap_width, graph->pixmap_height);
    cairo_set_source_rgba (cr, graph->background.blue, graph->background.green, graph->background.red, graph->background.alpha);
    cairo_fill (cr);

    /* Recompute pixmap */
    drawing_cursor = graph->ring_cursor;
    for (i = 0; i < graph->pixmap_width; i++)
    {
        /* Draw one bar of the graph. */
        if (graph->samples[drawing_cursor] != 0.0)
        {
            cairo_set_source_rgba (cr, graph->colours[graph->samp_states[drawing_cursor]].blue, graph->colours[graph->samp_states[drawing_cursor]].green,
                graph->colours[graph->samp_states[drawing_cursor]].red, graph->colours[graph->samp_states[drawing_cursor]].alpha);

            cairo_move_to (cr, i + 0.5, graph->pixmap_height);
            cairo_line_to (cr, i + 0.5, graph->pixmap_height - graph->samples[drawing_cursor] * graph->pixmap_height);
            cairo_stroke (cr);
        }

        /* Increment and wrap drawing cursor */
        drawing_cursor += 1;
        if (drawing_cursor >= graph->pixmap_width) drawing_cursor = 0;
    }

    /* Draw border in black */
    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_set_line_width (cr, 1);
    cairo_move_to (cr, 0, 0);
    cairo_line_to (cr, 0, graph->pixmap_height);
    cairo_line_to (cr, graph->pixmap_width, graph->pixmap_height);
    cairo_line_to (cr, graph->pixmap_width, 0);
    cairo_line_to (cr, 0, 0);
    cairo_stroke (cr);

    /* Apply label */
    fontsize = 12;
    if (graph->pixmap_width > 50) fontsize = graph->pixmap_height / 3;
    cairo_select_font_face (cr, "monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size (cr, fontsize);
    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_move_to (cr, (graph->pixmap_width >> 1) - ((fontsize * 5) / 4), ((graph->pixmap_height + fontsize) >> 1) - 1);
    cairo_show_text (cr, label);

    cairo_destroy (cr);

    /* Update image */
    pixbuf = gdk_pixbuf_new_from_data (cairo_image_surface_get_data (graph->pixmap), GDK_COLORSPACE_RGB, TRUE, 8,
        graph->pixmap_width, graph->pixmap_height, graph->pixmap_width * 4, NULL, NULL);
    gtk_image_set_from_pixbuf (GTK_IMAGE (graph->da), pixbuf);
}

/* Initialise graph for a particular size */

void graph_reload (PluginGraph *graph, int icon_size, GdkRGBA background, GdkRGBA foreground, GdkRGBA throttle1, GdkRGBA throttle2)
{
    /* Load colours */
    graph->background = background;
    graph->colours[0] = foreground;
    graph->colours[1] = throttle1;
    graph->colours[2] = throttle2;

    /* Allocate pixmap and statistics buffer without border pixels. */
    guint new_pixmap_height = icon_size - (BORDER_SIZE << 1);
    guint new_pixmap_width = (new_pixmap_height * 3) >> 1;
    if (new_pixmap_width < 50) new_pixmap_width = 50;

    if ((new_pixmap_width > 0) && (new_pixmap_height > 0))
    {
        /* If statistics buffer does not exist or it changed size, reallocate and preserve existing data. */
        if ((graph->samples == NULL) || (new_pixmap_width != graph->pixmap_width))
        {
            float *new_samples = g_new0 (float, new_pixmap_width);
            int *new_samp_states = g_new0 (int, new_pixmap_width);
            if (graph->samples != NULL)
            {
                if (new_pixmap_width > graph->pixmap_width)
                {
                    /* New allocation is larger. Introduce new "oldest" samples of zero following the cursor. */
                    memcpy (&new_samples[0], &graph->samples[0], graph->ring_cursor * sizeof (float));
                    memcpy (&new_samples[new_pixmap_width - graph->pixmap_width + graph->ring_cursor], &graph->samples[graph->ring_cursor], (graph->pixmap_width - graph->ring_cursor) * sizeof (float));
                    memcpy (&new_samp_states[0], &graph->samp_states[0], graph->ring_cursor * sizeof (int));
                    memcpy (&new_samp_states[new_pixmap_width - graph->pixmap_width + graph->ring_cursor], &graph->samp_states[graph->ring_cursor], (graph->pixmap_width - graph->ring_cursor) * sizeof (int));
                }
                else if (graph->ring_cursor <= new_pixmap_width)
                {
                    /* New allocation is smaller, but still larger than the ring buffer cursor. Discard the oldest samples following the cursor. */
                    memcpy (&new_samples[0], &graph->samples[0], graph->ring_cursor * sizeof (float));
                    memcpy (&new_samples[graph->ring_cursor], &graph->samples[graph->pixmap_width - new_pixmap_width + graph->ring_cursor], (new_pixmap_width - graph->ring_cursor) * sizeof (float));
                    memcpy (&new_samp_states[0], &graph->samp_states[0], graph->ring_cursor * sizeof (int));
                    memcpy (&new_samp_states[graph->ring_cursor], &graph->samp_states[graph->pixmap_width - new_pixmap_width + graph->ring_cursor], (new_pixmap_width - graph->ring_cursor) * sizeof (int));
                }
                else
                {
                    /* New allocation is smaller, and also smaller than the ring buffer cursor. Discard all oldest samples following the ring buffer cursor and additional samples at the beginning of the buffer. */
                    memcpy (&new_samples[0], &graph->samples[graph->ring_cursor - new_pixmap_width], new_pixmap_width * sizeof (float));
                    memcpy (&new_samp_states[0], &graph->samp_states[graph->ring_cursor - new_pixmap_width], new_pixmap_width * sizeof (int));
                    graph->ring_cursor = 0;
                }
                g_free (graph->samples);
                g_free (graph->samp_states);
            }
            graph->samples = new_samples;
            graph->samp_states = new_samp_states;
        }

        /* Allocate or reallocate pixmap. */
        graph->pixmap_width = new_pixmap_width;
        graph->pixmap_height = new_pixmap_height;
        if (graph->pixmap) cairo_surface_destroy (graph->pixmap);
        graph->pixmap = cairo_image_surface_create (CAIRO_FORMAT_RGB24, graph->pixmap_width, graph->pixmap_height);

        /* Redraw pixmap at the new size. */
        graph_redraw (graph, "");
    }
}

/* Add new data point to the graph */

void graph_new_point (PluginGraph *graph, float value, int state, char *label)
{
    if (value < 0.0) value = 0.0;
    else if (value > 1.0) value = 1.0;
    graph->samples[graph->ring_cursor] = value;
    graph->samp_states[graph->ring_cursor] = state;

    graph->ring_cursor += 1;
    if (graph->ring_cursor >= graph->pixmap_width) graph->ring_cursor = 0;

    graph_redraw (graph, label);
}

void graph_init (PluginGraph *graph)
{
    graph->da = gtk_image_new ();
    graph->samples = NULL;
    graph->ring_cursor = 0;
    graph->pixmap = NULL;
}

void graph_free (PluginGraph *graph)
{
    if (graph->pixmap) cairo_surface_destroy (graph->pixmap);
    if (graph->samples) g_free (graph->samples);
    if (graph->samp_states) g_free (graph->samp_states);
    gtk_widget_destroy (graph->da);
}

/*----------------------------------------------------------------------------*/
/* Click-away pop-up */
/*----------------------------------------------------------------------------*/

static gboolean popup_mapped (GtkWidget *widget, GdkEvent *, gpointer)
{
    gdk_seat_grab (gdk_display_get_default_seat (gdk_display_get_default ()), gtk_widget_get_window (widget), GDK_SEAT_CAPABILITY_ALL_POINTING, TRUE, NULL, NULL, NULL, NULL);
    return FALSE;
}

static gboolean popup_button_press (GtkWidget *widget, GdkEventButton *event, gpointer)
{
    int x, y;
    gtk_window_get_size (GTK_WINDOW (widget), &x, &y);
    if (event->x < 0 || event->y < 0 || event->x > x || event->y > y)
    {
        if (widget) gtk_widget_destroy (widget);
        gdk_seat_ungrab (gdk_display_get_default_seat (gdk_display_get_default ()));
    }
    return FALSE;
}

void popup_at_button (LXPanel *panel, GtkWidget *window, GtkWidget *button, gpointer plugin)
{
    gint x, y;
    gtk_window_set_decorated (GTK_WINDOW (window), FALSE);
    gtk_window_set_type_hint (GTK_WINDOW (window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_skip_taskbar_hint (GTK_WINDOW (window), TRUE);
    gtk_window_set_position (GTK_WINDOW (window), GTK_WIN_POS_MOUSE);
    gtk_widget_show_all (window);
    gtk_widget_hide (window);
    lxpanel_plugin_popup_set_position_helper (panel, button, window, &x, &y);
    gtk_widget_show_all (window);
    gtk_window_present (GTK_WINDOW (window));
    gdk_window_move (gtk_widget_get_window (window), x, y);
    g_signal_connect (G_OBJECT (window), "map-event", G_CALLBACK (popup_mapped), plugin);
    g_signal_connect (G_OBJECT (window), "button-press-event", G_CALLBACK (popup_button_press), plugin);
}

gboolean is_pi (void)
{
    if (!access ("/boot/firmware/config.txt", R_OK)) return TRUE;
    return FALSE;
}
