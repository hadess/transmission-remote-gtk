/*
 * transmission-remote-gtk - Transmission RPC client for GTK
 * Copyright (C) 2011  Alan Fitton

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* Most of the UI code was taken from open-dialog.c and files-list.c
 * in Transmission, adapted to fit in with different torrent file parser
 * and JSON dispatch.
 */

#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <glib/gprintf.h>
#include <gconf/gconf-client.h>

#include "hig.h"
#include "util.h"
#include "trg-client.h"
#include "trg-main-window.h"
#include "trg-file-parser.h"
#include "trg-torrent-add-dialog.h"
#include "trg-cell-renderer-size.h"
#include "trg-preferences.h"
#include "requests.h"
#include "torrent.h"
#include "json.h"
#include "dispatch.h"
#include "protocol-constants.h"

enum {
    PROP_0,
    PROP_FILENAME,
    PROP_PARENT,
    PROP_CLIENT
};

enum {
    FC_ICON,
    FC_INDEX,
    FC_LABEL,
    FC_SIZE,
    FC_PRIORITY,
    FC_ENABLED,
    N_FILE_COLS
};

enum {
    NOT_SET = 1000,
    MIXED = 1001
};

#define TR_COLUMN_ID_KEY "tr-id-key"

G_DEFINE_TYPE(TrgTorrentAddDialog, trg_torrent_add_dialog, GTK_TYPE_DIALOG)
#define TRG_TORRENT_ADD_DIALOG_GET_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), TRG_TYPE_TORRENT_ADD_DIALOG, TrgTorrentAddDialogPrivate))
typedef struct _TrgTorrentAddDialogPrivate
 TrgTorrentAddDialogPrivate;

struct _TrgTorrentAddDialogPrivate {
    trg_client *client;
    TrgMainWindow *parent;
    GSList *filenames;
    GtkWidget *source_chooser;
    GtkWidget *dest_combo;
    GtkWidget *priority_combo;
    GtkWidget *file_list;
    GtkTreeStore *store;
    GtkWidget *paused_check;
};

static void
trg_torrent_add_dialog_set_property(GObject * object,
                                    guint prop_id,
                                    const GValue * value,
                                    GParamSpec * pspec G_GNUC_UNUSED)
{
    TrgTorrentAddDialogPrivate *priv =
        TRG_TORRENT_ADD_DIALOG_GET_PRIVATE(object);

    switch (prop_id) {
    case PROP_FILENAME:
        priv->filenames = g_value_get_pointer(value);
        break;
    case PROP_PARENT:
        priv->parent = g_value_get_object(value);
        break;
    case PROP_CLIENT:
        priv->client = g_value_get_pointer(value);
        break;
    }
}

static void
trg_torrent_add_dialog_get_property(GObject * object,
                                    guint prop_id,
                                    GValue * value,
                                    GParamSpec * pspec G_GNUC_UNUSED)
{
    TrgTorrentAddDialogPrivate *priv =
        TRG_TORRENT_ADD_DIALOG_GET_PRIVATE(object);

    switch (prop_id) {
    case PROP_FILENAME:
        g_value_set_pointer(value, priv->filenames);
        break;
    case PROP_PARENT:
        g_value_set_object(value, priv->parent);
        break;
    }
}

/* Use synchronous dispatch() in our dedicated thread function.
 * This means torrents are added in sequence, instead of dispatch_async()
 * working concurrently for each upload.
 */

struct add_torrent_threadfunc_args {
    GSList *list;
    trg_client *client;
    gpointer cb_data;
    gboolean paused;
    gchar *dir;
    gint priority;
    gboolean extraArgs;
};

static void add_set_common_args(JsonObject * args, gint priority,
                                gchar * dir)
{
    json_object_set_string_member(args, FIELD_FILE_DOWNLOAD_DIR, dir);
    json_object_set_int_member(args, FIELD_BANDWIDTH_PRIORITY,
                               (gint64) priority);
}

static gpointer add_files_threadfunc(gpointer data)
{
    struct add_torrent_threadfunc_args *files_thread_data =
        (struct add_torrent_threadfunc_args *) data;
    GSList *li;

    for (li = files_thread_data->list; li != NULL; li = g_slist_next(li)) {
        JsonNode *request =
            torrent_add((gchar *) li->data, files_thread_data->paused);
        JsonObject *args = node_get_arguments(request);
        JsonObject *response;
        gint status;

        if (files_thread_data->extraArgs)
            add_set_common_args(args, files_thread_data->priority,
                                files_thread_data->dir);

        response = dispatch(files_thread_data->client, request, &status);
        on_generic_interactive_action(response, status,
                                      files_thread_data->cb_data);
    }

    g_str_slist_free(files_thread_data->list);

    if (files_thread_data->extraArgs)
        g_free(files_thread_data->dir);

    g_free(files_thread_data);

    return NULL;
}

static gchar *trg_destination_folder_get(GtkComboBox * box)
{
    return gtk_combo_box_get_active_text(box);
}

static GtkWidget *trg_destination_folder_new(trg_client * client)
{
    const gchar *defaultDownDir = json_object_get_string_member(client->session, SGET_DOWNLOAD_DIR);
    GtkWidget *combo = gtk_combo_box_entry_new_text();
    int i;
    GSList *dirs = NULL;
    GSList *li;

    g_slist_str_set_add(&dirs, defaultDownDir);

    g_mutex_lock(client->updateMutex);
    for (i = 0; i < json_array_get_length(client->torrents); i++) {
        JsonObject *t = json_node_get_object(json_array_get_element(client->torrents, i));
        const gchar *dd = torrent_get_download_dir(t);
        if (dd) {
            g_printf("dd: %s\n", dd);
            g_slist_str_set_add(&dirs, dd);
        }
    }
    g_mutex_unlock(client->updateMutex);

    for (li = dirs; li != NULL; li = g_slist_next(li))
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), (gchar*)li->data);

    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    g_str_slist_free(dirs);

    return combo;
}

static void launch_add_thread(struct add_torrent_threadfunc_args *args)
{
    GError *error = NULL;
    g_thread_create(add_files_threadfunc, args, FALSE, &error);

    if (error != NULL) {
        g_printf("thread creation error: %s\n", error->message);
        g_error_free(error);
        g_str_slist_free(args->list);
        g_free(args);
    }
}

static gboolean add_file_indexes_foreachfunc(GtkTreeModel * model,
                                             GtkTreePath *
                                             path G_GNUC_UNUSED,
                                             GtkTreeIter * iter,
                                             gpointer data)
{
    JsonObject *args = (JsonObject *) data;
    gint priority, index, wanted;

    gtk_tree_model_get(model, iter, FC_PRIORITY, &priority,
                       FC_ENABLED, &wanted, FC_INDEX, &index, -1);

    if (gtk_tree_model_iter_has_child(model, iter))
        return FALSE;

    if (wanted)
        add_file_id_to_array(args, FIELD_FILES_WANTED, index);
    else
        add_file_id_to_array(args, FIELD_FILES_UNWANTED, index);

    if (priority == TR_PRI_LOW)
        add_file_id_to_array(args, FIELD_FILES_PRIORITY_LOW, index);
    else if (priority == TR_PRI_HIGH)
        add_file_id_to_array(args, FIELD_FILES_PRIORITY_HIGH, index);
    else
        add_file_id_to_array(args, FIELD_FILES_PRIORITY_NORMAL, index);

    return FALSE;
}

static void
trg_torrent_add_response_cb(GtkDialog * dlg, gint res_id, gpointer data)
{
    TrgTorrentAddDialogPrivate *priv =
        TRG_TORRENT_ADD_DIALOG_GET_PRIVATE(dlg);

    if (res_id == GTK_RESPONSE_ACCEPT) {
        gboolean paused =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON
                                         (priv->paused_check));
        gint priority =
            gtk_combo_box_get_active(GTK_COMBO_BOX(priv->priority_combo)) -
            1;
        gchar *dir =
            trg_destination_folder_get(GTK_COMBO_BOX(priv->dest_combo));

        if (g_slist_length(priv->filenames) == 1) {
            JsonNode *req =
                torrent_add((gchar *) priv->filenames->data, paused);
            JsonObject *args = node_get_arguments(req);
            gtk_tree_model_foreach(GTK_TREE_MODEL(priv->store),
                                   add_file_indexes_foreachfunc, args);
            add_set_common_args(args, priority, dir);
            dispatch_async(priv->client, req,
                           on_generic_interactive_action, priv->parent);
            g_str_slist_free(priv->filenames);
        } else {
            struct add_torrent_threadfunc_args *args =
                g_new(struct add_torrent_threadfunc_args, 1);
            args->list = priv->filenames;
            args->cb_data = data;
            args->client = priv->client;
            args->dir = g_strdup(dir);
            args->priority = priority;
            args->paused = paused;
            args->extraArgs = TRUE;

            launch_add_thread(args);
        }
    } else {
        g_str_slist_free(priv->filenames);
    }

    gtk_widget_destroy(GTK_WIDGET(dlg));
}

static void
renderPriority(GtkTreeViewColumn * column G_GNUC_UNUSED,
               GtkCellRenderer * renderer,
               GtkTreeModel * model,
               GtkTreeIter * iter, gpointer data G_GNUC_UNUSED)
{
    int priority;
    const char *text;
    gtk_tree_model_get(model, iter, FC_PRIORITY, &priority, -1);
    switch (priority) {
    case TR_PRI_HIGH:
        text = _("High");
        break;
    case TR_PRI_NORMAL:
        text = _("Normal");
        break;
    case TR_PRI_LOW:
        text = _("Low");
        break;
    default:
        text = _("Mixed");
        break;
    }
    g_object_set(renderer, "text", text, NULL);
}

static void
renderDownload(GtkTreeViewColumn * column G_GNUC_UNUSED,
               GtkCellRenderer * renderer,
               GtkTreeModel * model,
               GtkTreeIter * iter, gpointer data G_GNUC_UNUSED)
{
    gint enabled;
    gtk_tree_model_get(model, iter, FC_ENABLED, &enabled, -1);
    g_object_set(renderer, "inconsistent", (enabled == MIXED),
                 "active", (enabled == TRUE), NULL);
}

struct SubtreeForeachData {
    gint column;
    gint new_value;
    GtkTreePath *path;
};

static gboolean
setSubtreeForeach(GtkTreeModel * model,
                  GtkTreePath * path, GtkTreeIter * iter, gpointer gdata)
{
    /*const gboolean is_file = !gtk_tree_model_iter_has_child(model, iter); */

    struct SubtreeForeachData *data = gdata;

    if (!gtk_tree_path_compare(path, data->path)
        || gtk_tree_path_is_descendant(path, data->path)) {
        gtk_tree_store_set(GTK_TREE_STORE(model), iter, data->column,
                           data->new_value, -1);
    }

    return FALSE;               /* keep walking */
}

static void
setSubtree(GtkTreeModel * model, GtkTreePath * path, GtkTreeIter * iter,
           gint column, gint new_value)
{
    gint result = new_value;
    GtkTreeIter back_iter = *iter;

    if (gtk_tree_model_iter_has_child(model, iter)) {
        struct SubtreeForeachData tmp;
        GtkTreeIter parent;

        tmp.column = column;
        tmp.new_value = new_value;
        tmp.path = path;
        gtk_tree_model_foreach(model, setSubtreeForeach, &tmp);

        gtk_tree_model_iter_parent(model, &parent, iter);
    } else {
        gtk_tree_store_set(GTK_TREE_STORE(model), &back_iter, column,
                           new_value, -1);
    }

    while (1) {
        GtkTreeIter tmp_iter;
        gint n_children, i;

        if (!gtk_tree_model_iter_parent(model, &tmp_iter, &back_iter))
            break;

        n_children = gtk_tree_model_iter_n_children(model, &tmp_iter);

        for (i = 0; i < n_children; i++) {
            GtkTreeIter child;
            gint current_value;

            if (!gtk_tree_model_iter_nth_child
                (model, &child, &tmp_iter, i))
                continue;

            gtk_tree_model_get(model, &child, column, &current_value, -1);
            if (current_value != new_value) {
                result = MIXED;
                break;
            }
        }

        gtk_tree_store_set(GTK_TREE_STORE(model), &tmp_iter, column,
                           result, -1);

        back_iter = tmp_iter;
    }
}

static gboolean
onViewPathToggled(GtkTreeView * view,
                  GtkTreeViewColumn * col,
                  GtkTreePath * path, gpointer data)
{
    int cid;
    gboolean handled = FALSE;

    if (!col || !path)
        return FALSE;

    cid =
        GPOINTER_TO_INT(g_object_get_data
                        (G_OBJECT(col), TR_COLUMN_ID_KEY));
    if ((cid == FC_PRIORITY) || (cid == FC_ENABLED)) {
        GtkTreeIter iter;
        /*GArray *indices = getActiveFilesForPath(view, path); */
        GtkTreeModel *model = gtk_tree_view_get_model(view);

        gtk_tree_model_get_iter(model, &iter, path);

        if (cid == FC_PRIORITY) {
            int priority;
            gtk_tree_model_get(model, &iter, FC_PRIORITY, &priority, -1);
            switch (priority) {
            case TR_PRI_NORMAL:
                priority = TR_PRI_HIGH;
                break;
            case TR_PRI_HIGH:
                priority = TR_PRI_LOW;
                break;
            default:
                priority = TR_PRI_NORMAL;
                break;
            }
            setSubtree(model, path, &iter, FC_PRIORITY, priority);
        } else {
            int enabled;
            gtk_tree_model_get(model, &iter, FC_ENABLED, &enabled, -1);
            enabled = !enabled;

            setSubtree(model, path, &iter, FC_ENABLED, enabled);
        }

        handled = TRUE;
    }

    return handled;
}

static gboolean
getAndSelectEventPath(GtkTreeView * treeview,
                      GdkEventButton * event,
                      GtkTreeViewColumn ** col, GtkTreePath ** path)
{
    GtkTreeSelection *sel;

    if (gtk_tree_view_get_path_at_pos(treeview,
                                      event->x, event->y,
                                      path, col, NULL, NULL)) {
        sel = gtk_tree_view_get_selection(treeview);
        if (!gtk_tree_selection_path_is_selected(sel, *path)) {
            gtk_tree_selection_unselect_all(sel);
            gtk_tree_selection_select_path(sel, *path);
        }
        return TRUE;
    }

    return FALSE;
}

static gboolean
onViewButtonPressed(GtkWidget * w, GdkEventButton * event, gpointer gdata)
{
    GtkTreeViewColumn *col = NULL;
    GtkTreePath *path = NULL;
    gboolean handled = FALSE;
    GtkTreeView *treeview = GTK_TREE_VIEW(w);

    if (event->type == GDK_BUTTON_PRESS && event->button == 1
        && !(event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))
        && getAndSelectEventPath(treeview, event, &col, &path)) {
        handled = onViewPathToggled(treeview, col, path, NULL);
    }

    gtk_tree_path_free(path);
    return handled;
}

GtkWidget *gtr_file_list_new(GtkTreeStore ** store)
{
    int size;
    int width;
    GtkWidget *view;
    GtkWidget *scroll;
    GtkCellRenderer *rend;
    GtkTreeSelection *sel;
    GtkTreeViewColumn *col;
    GtkTreeView *tree_view;
    const char *title;
    PangoLayout *pango_layout;
    PangoContext *pango_context;
    PangoFontDescription *pango_font_description;

    /* create the view */
    view = gtk_tree_view_new();
    tree_view = GTK_TREE_VIEW(view);
    gtk_tree_view_set_rules_hint(tree_view, TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(view), GUI_PAD_BIG);
    g_signal_connect(view, "button-press-event",
                     G_CALLBACK(onViewButtonPressed), view);

    pango_context = gtk_widget_create_pango_context(view);
    pango_font_description =
        pango_font_description_copy(pango_context_get_font_description
                                    (pango_context));
    size = pango_font_description_get_size(pango_font_description);
    pango_font_description_set_size(pango_font_description, size * 0.8);
    g_object_unref(G_OBJECT(pango_context));

    /* set up view */
    sel = gtk_tree_view_get_selection(tree_view);
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
    gtk_tree_view_expand_all(tree_view);
    gtk_tree_view_set_search_column(tree_view, FC_LABEL);

    /* add file column */
    col = GTK_TREE_VIEW_COLUMN(g_object_new(GTK_TYPE_TREE_VIEW_COLUMN,
                                            "expand", TRUE,
                                            "title", _("Name"), NULL));
    gtk_tree_view_column_set_resizable(col, TRUE);
    rend = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, rend, FALSE);
    gtk_tree_view_column_add_attribute(col, rend, "stock-id", FC_ICON);

    /* add text renderer */
    rend = gtk_cell_renderer_text_new();
    g_object_set(rend, "ellipsize", PANGO_ELLIPSIZE_END, "font-desc",
                 pango_font_description, NULL);
    gtk_tree_view_column_pack_start(col, rend, TRUE);
    gtk_tree_view_column_set_attributes(col, rend, "text", FC_LABEL, NULL);
    gtk_tree_view_column_set_sort_column_id(col, FC_LABEL);
    gtk_tree_view_append_column(tree_view, col);

    /* add "size" column */

    title = _("Size");
    rend = trg_cell_renderer_size_new();
    g_object_set(rend, "alignment", PANGO_ALIGN_RIGHT,
                 "font-desc", pango_font_description,
                 "xpad", GUI_PAD, "xalign", 1.0f, "yalign", 0.5f, NULL);
    col = gtk_tree_view_column_new_with_attributes(title, rend, NULL);
    gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_GROW_ONLY);
    gtk_tree_view_column_set_sort_column_id(col, FC_SIZE);
    gtk_tree_view_column_set_attributes(col, rend, "size-value", FC_SIZE,
                                        NULL);
    gtk_tree_view_append_column(tree_view, col);

    /* add "enabled" column */
    title = _("Download");
    pango_layout = gtk_widget_create_pango_layout(view, title);
    pango_layout_get_pixel_size(pango_layout, &width, NULL);
    width += 30;                /* room for the sort indicator */
    g_object_unref(G_OBJECT(pango_layout));
    rend = gtk_cell_renderer_toggle_new();
    col = gtk_tree_view_column_new_with_attributes(title, rend, NULL);
    g_object_set_data(G_OBJECT(col), TR_COLUMN_ID_KEY,
                      GINT_TO_POINTER(FC_ENABLED));
    gtk_tree_view_column_set_fixed_width(col, width);
    gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_cell_data_func(col, rend, renderDownload,
                                            NULL, NULL);
    gtk_tree_view_column_set_sort_column_id(col, FC_ENABLED);
    gtk_tree_view_append_column(tree_view, col);

    /* add priority column */
    title = _("Priority");
    pango_layout = gtk_widget_create_pango_layout(view, title);
    pango_layout_get_pixel_size(pango_layout, &width, NULL);
    width += 30;                /* room for the sort indicator */
    g_object_unref(G_OBJECT(pango_layout));
    rend = gtk_cell_renderer_text_new();
    g_object_set(rend, "xalign", (gfloat) 0.5, "yalign", (gfloat) 0.5,
                 NULL);
    col = gtk_tree_view_column_new_with_attributes(title, rend, NULL);
    g_object_set_data(G_OBJECT(col), TR_COLUMN_ID_KEY,
                      GINT_TO_POINTER(FC_PRIORITY));
    gtk_tree_view_column_set_fixed_width(col, width);
    gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_sort_column_id(col, FC_PRIORITY);
    gtk_tree_view_column_set_cell_data_func(col, rend, renderPriority,
                                            NULL, NULL);
    gtk_tree_view_append_column(tree_view, col);

    *store = gtk_tree_store_new(N_FILE_COLS, G_TYPE_STRING,     /* icon */
                                G_TYPE_UINT,    /* index */
                                G_TYPE_STRING,  /* label */
                                G_TYPE_INT64,   /* size */
                                G_TYPE_INT,     /* priority */
                                G_TYPE_INT);    /* dl enabled */

    gtk_tree_view_set_model(tree_view, GTK_TREE_MODEL(*store));

    /* create the scrolled window and stick the view in it */
    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll),
                                        GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_widget_set_size_request(scroll, -1, 200);

    pango_font_description_free(pango_font_description);
    return scroll;
}

static GtkWidget *gtr_dialog_get_content_area(GtkDialog * dialog)
{
#if GTK_CHECK_VERSION( 2,14,0 )
    return gtk_dialog_get_content_area(dialog);
#else
    return dialog->vbox;
#endif
}

static void gtr_dialog_set_content(GtkDialog * dialog, GtkWidget * content)
{
    GtkWidget *vbox = gtr_dialog_get_content_area(dialog);
    gtk_box_pack_start(GTK_BOX(vbox), content, TRUE, TRUE, 0);
    gtk_widget_show_all(content);
}

GtkWidget *gtr_combo_box_new_enum(const char *text_1, ...)
{
    GtkWidget *w;
    GtkCellRenderer *r;
    GtkListStore *store;
    va_list vl;
    const char *text;
    va_start(vl, text_1);

    store = gtk_list_store_new(2, G_TYPE_INT, G_TYPE_STRING);

    text = text_1;
    if (text != NULL)
        do {
            const int val = va_arg(vl, int);
            gtk_list_store_insert_with_values(store, NULL, INT_MAX, 0, val,
                                              1, text, -1);
            text = va_arg(vl, const char *);
        }
        while (text != NULL);

    w = gtk_combo_box_new_with_model(GTK_TREE_MODEL(store));
    r = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(w), r, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(w), r, "text", 1, NULL);

    /* cleanup */
    g_object_unref(store);
    return w;
}

GtkWidget *gtr_priority_combo_new(void)
{
    return gtr_combo_box_new_enum(_("Low"), TR_PRI_LOW,
                                  _("Normal"), TR_PRI_NORMAL,
                                  _("High"), TR_PRI_HIGH, NULL);
}

static void addTorrentFilters(GtkFileChooser * chooser)
{
    GtkFileFilter *filter;

    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, _("Torrent files"));
    gtk_file_filter_add_pattern(filter, "*.torrent");
    gtk_file_chooser_add_filter(chooser, filter);

    filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, _("All files"));
    gtk_file_filter_add_pattern(filter, "*");
    gtk_file_chooser_add_filter(chooser, filter);
}

static void store_add_node(GtkTreeStore * store, GtkTreeIter * parent,
                           trg_torrent_file_node * node)
{
    GtkTreeIter child;
    GList *li;

    if (node->name) {
        gtk_tree_store_append(store, &child, parent);
        gtk_tree_store_set(store, &child, FC_LABEL, node->name, -1);
        gtk_tree_store_set(store, &child, FC_ICON,
                           node->children ? GTK_STOCK_DIRECTORY :
                           GTK_STOCK_FILE, -1);
        gtk_tree_store_set(store, &child, FC_ENABLED, 1, -1);
        if (!node->children) {
            gtk_tree_store_set(store, &child, FC_INDEX, node->index, -1);
            gtk_tree_store_set(store, &child, FC_SIZE, node->length, -1);
            gtk_tree_store_set(store, &child, FC_PRIORITY, 0, -1);
        }
    }

    for (li = node->children; li != NULL; li = g_list_next(li))
        store_add_node(store, node->name ? &child : NULL,
                       (trg_torrent_file_node *) li->data);
}

static void trg_torrent_add_dialog_set_filenames(TrgTorrentAddDialog * d,
                                                 GSList * filenames)
{
    TrgTorrentAddDialogPrivate *priv =
        TRG_TORRENT_ADD_DIALOG_GET_PRIVATE(d);
    GtkButton *chooser = GTK_BUTTON(priv->source_chooser);
    gint nfiles = filenames ? g_slist_length(filenames) : 0;

    gtk_tree_store_clear(priv->store);

    if (nfiles == 1) {
        gchar *file_name = (gchar *) filenames->data;
        gchar *file_name_base = g_path_get_basename(file_name);
        trg_torrent_file *tor_data = trg_parse_torrent_file(file_name);
        store_add_node(priv->store, NULL, tor_data->top_node);
        trg_torrent_file_free(tor_data);
        gtk_button_set_label(chooser, file_name_base);
        g_free(file_name_base);
        gtk_widget_set_sensitive(priv->file_list, TRUE);
    } else {
        gtk_widget_set_sensitive(priv->file_list, FALSE);
        if (nfiles < 1) {
            gtk_button_set_label(chooser, _("(None)"));
        } else {
            gtk_button_set_label(chooser, _("(Multiple)"));
        }
    }

    priv->filenames = filenames;
}

static void trg_torrent_add_dialog_generic_save_dir(GtkFileChooser * c,
                                                    GConfClient * gcc)
{
    gchar *cwd = gtk_file_chooser_get_current_folder(c);

    if (cwd) {
        gconf_client_set_string(gcc, TRG_GCONF_KEY_LAST_TORRENT_DIR, cwd,
                                NULL);
        g_free(cwd);
    }
}

static GtkWidget *trg_torrent_add_dialog_generic(GtkWindow * parent,
                                                 GConfClient * gcc)
{
    GtkWidget *w = gtk_file_chooser_dialog_new(_("Add a Torrent"), parent,
                                               GTK_FILE_CHOOSER_ACTION_OPEN,
                                               GTK_STOCK_CANCEL,
                                               GTK_RESPONSE_CANCEL,
                                               GTK_STOCK_ADD,
                                               GTK_RESPONSE_ACCEPT,
                                               NULL);
    gchar *dir =
        gconf_client_get_string(gcc, TRG_GCONF_KEY_LAST_TORRENT_DIR, NULL);
    if (dir) {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(w), dir);
        g_free(dir);
    }
    addTorrentFilters(GTK_FILE_CHOOSER(w));
    gtk_dialog_set_alternative_button_order(GTK_DIALOG(w),
                                            GTK_RESPONSE_ACCEPT,
                                            GTK_RESPONSE_CANCEL, -1);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(w), TRUE);
    return w;
}

static void trg_torrent_add_dialog_source_click_cb(GtkWidget * w,
                                                   gpointer data)
{
    TrgTorrentAddDialogPrivate *priv =
        TRG_TORRENT_ADD_DIALOG_GET_PRIVATE(data);
    GtkWidget *d = trg_torrent_add_dialog_generic(GTK_WINDOW(data),
                                                  priv->client->gconf);

    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        if (priv->filenames)
            g_str_slist_free(priv->filenames);

        priv->filenames =
            gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(d));

        trg_torrent_add_dialog_generic_save_dir(GTK_FILE_CHOOSER(d),
                                                priv->client->gconf);
        trg_torrent_add_dialog_set_filenames(TRG_TORRENT_ADD_DIALOG(data),
                                             priv->filenames);
    }

    gtk_widget_destroy(GTK_WIDGET(d));
}

static GObject *trg_torrent_add_dialog_constructor(GType type,
                                                   guint
                                                   n_construct_properties,
                                                   GObjectConstructParam
                                                   * construct_params)
{
    GObject *obj = G_OBJECT_CLASS
        (trg_torrent_add_dialog_parent_class)->constructor(type,
                                                           n_construct_properties,
                                                           construct_params);
    TrgTorrentAddDialogPrivate *priv =
        TRG_TORRENT_ADD_DIALOG_GET_PRIVATE(obj);

    GtkWidget *t, *l;
    gint row = 0;
    gint col = 0;

    /* window */
    gtk_window_set_title(GTK_WINDOW(obj), _("Add Torrent"));
    gtk_window_set_transient_for(GTK_WINDOW(obj),
                                 GTK_WINDOW(priv->parent));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(obj), TRUE);

    /* buttons */
    gtk_dialog_add_button(GTK_DIALOG(obj), GTK_STOCK_CANCEL,
                          GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(obj), GTK_STOCK_OPEN,
                          GTK_RESPONSE_ACCEPT);
    gtk_dialog_set_alternative_button_order(GTK_DIALOG(obj),
                                            GTK_RESPONSE_ACCEPT,
                                            GTK_RESPONSE_CANCEL, -1);
    gtk_dialog_set_default_response(GTK_DIALOG(obj), GTK_RESPONSE_ACCEPT);

    /* workspace */
    t = gtk_table_new(6, 2, FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(t), GUI_PAD_BIG);
    gtk_table_set_row_spacings(GTK_TABLE(t), GUI_PAD);
    gtk_table_set_col_spacings(GTK_TABLE(t), GUI_PAD_BIG);

    priv->file_list = gtr_file_list_new(&priv->store);
    gtk_widget_set_sensitive(priv->file_list, FALSE);
    priv->paused_check =
        gtk_check_button_new_with_mnemonic(_("Start _paused"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->paused_check),
                                 pref_get_start_paused(priv->
                                                       client->gconf));

    priv->priority_combo = gtr_priority_combo_new();
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->priority_combo), 1);

    l = gtk_label_new_with_mnemonic(_("_Torrent file:"));
    gtk_misc_set_alignment(GTK_MISC(l), 0.0f, 0.5f);
    gtk_table_attach(GTK_TABLE(t), l, col, col + 1, row, row + 1, GTK_FILL,
                     0, 0, 0);
    ++col;

    priv->source_chooser = gtk_button_new();
    gtk_button_set_alignment(GTK_BUTTON(priv->source_chooser), 0.0f, 0.5f);
    trg_torrent_add_dialog_set_filenames(TRG_TORRENT_ADD_DIALOG(obj),
                                         priv->filenames);

    gtk_table_attach(GTK_TABLE(t), priv->source_chooser, col, col + 1, row,
                     row + 1, ~0, 0, 0, 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), priv->source_chooser);
    g_signal_connect(priv->source_chooser, "clicked",
                     G_CALLBACK(trg_torrent_add_dialog_source_click_cb),
                     obj);

    ++row;
    col = 0;
    l = gtk_label_new_with_mnemonic(_("_Destination folder:"));
    gtk_misc_set_alignment(GTK_MISC(l), 0.0f, 0.5f);
    gtk_table_attach(GTK_TABLE(t), l, col, col + 1, row, row + 1, GTK_FILL,
                     0, 0, 0);
    ++col;
    priv->dest_combo = trg_destination_folder_new(priv->client);

    /*if( !gtk_file_chooser_set_current_folder( GTK_FILE_CHOOSER( w ),
       data->downloadDir ) )
       g_warning( "couldn't select '%s'", data->downloadDir );
       list = get_recent_destinations( );
       for( walk = list; walk; walk = walk->next )
       gtk_file_chooser_add_shortcut_folder( GTK_FILE_CHOOSER( w ), walk->data, NULL );
       g_slist_free( list ); */
    gtk_table_attach(GTK_TABLE(t), priv->dest_combo, col, col + 1, row,
                     row + 1, ~0, 0, 0, 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), priv->dest_combo);

    ++row;
    col = 0;
    gtk_widget_set_size_request(priv->file_list, 466u, 300u);
    gtk_table_attach_defaults(GTK_TABLE(t), priv->file_list, col, col + 2,
                              row, row + 1);

    ++row;
    col = 0;
    l = gtk_label_new_with_mnemonic(_("Torrent _priority:"));
    gtk_misc_set_alignment(GTK_MISC(l), 0.0f, 0.5f);
    gtk_table_attach(GTK_TABLE(t), l, col, col + 1, row, row + 1, ~0, 0, 0,
                     0);
    ++col;
    gtk_table_attach(GTK_TABLE(t), priv->priority_combo, col, col + 1, row,
                     row + 1, ~0, 0, 0, 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), priv->priority_combo);

    ++row;
    col = 0;
    gtk_table_attach(GTK_TABLE(t), priv->paused_check, col, col + 2, row,
                     row + 1, GTK_FILL, 0, 0, 0);

    gtr_dialog_set_content(GTK_DIALOG(obj), t);

    g_signal_connect(G_OBJECT(obj),
                     "response",
                     G_CALLBACK(trg_torrent_add_response_cb),
                     priv->parent);

    return obj;
}

static void
trg_torrent_add_dialog_class_init(TrgTorrentAddDialogClass * klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    g_type_class_add_private(klass, sizeof(TrgTorrentAddDialogPrivate));

    object_class->set_property = trg_torrent_add_dialog_set_property;
    object_class->get_property = trg_torrent_add_dialog_get_property;
    object_class->constructor = trg_torrent_add_dialog_constructor;

    g_object_class_install_property(object_class,
                                    PROP_FILENAME,
                                    g_param_spec_pointer("filenames",
                                                         "filenames",
                                                         "filenames",
                                                         G_PARAM_READWRITE
                                                         |
                                                         G_PARAM_CONSTRUCT_ONLY
                                                         |
                                                         G_PARAM_STATIC_NAME
                                                         |
                                                         G_PARAM_STATIC_NICK
                                                         |
                                                         G_PARAM_STATIC_BLURB));

    g_object_class_install_property(object_class,
                                    PROP_CLIENT,
                                    g_param_spec_pointer("client",
                                                         "client",
                                                         "client",
                                                         G_PARAM_READWRITE
                                                         |
                                                         G_PARAM_CONSTRUCT_ONLY
                                                         |
                                                         G_PARAM_STATIC_NAME
                                                         |
                                                         G_PARAM_STATIC_NICK
                                                         |
                                                         G_PARAM_STATIC_BLURB));

    g_object_class_install_property(object_class,
                                    PROP_PARENT,
                                    g_param_spec_object("parent", "parent",
                                                        "parent",
                                                        TRG_TYPE_MAIN_WINDOW,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY
                                                        |
                                                        G_PARAM_STATIC_NAME
                                                        |
                                                        G_PARAM_STATIC_NICK
                                                        |
                                                        G_PARAM_STATIC_BLURB));
}

static void trg_torrent_add_dialog_init(TrgTorrentAddDialog * self)
{
}

TrgTorrentAddDialog *trg_torrent_add_dialog_new(TrgMainWindow * parent,
                                                trg_client * client,
                                                GSList * filenames)
{
    return g_object_new(TRG_TYPE_TORRENT_ADD_DIALOG,
                        "filenames", filenames,
                        "parent", parent, "client", client, NULL);
}

void trg_torrent_add_dialog(TrgMainWindow * win, trg_client * client)
{
    GtkWidget *w;
    GtkWidget *c;

    w = trg_torrent_add_dialog_generic(GTK_WINDOW(win), client->gconf);

    c = gtk_check_button_new_with_mnemonic(_("Show _options dialog"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c),
                                 pref_get_add_options_dialog
                                 (client->gconf));
    gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(w), c);

    if (gtk_dialog_run(GTK_DIALOG(w)) == GTK_RESPONSE_ACCEPT) {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(w);
        GtkToggleButton *tb =
            GTK_TOGGLE_BUTTON(gtk_file_chooser_get_extra_widget(chooser));
        gboolean showOptions = gtk_toggle_button_get_active(tb);
        GSList *l = gtk_file_chooser_get_filenames(chooser);

        trg_torrent_add_dialog_generic_save_dir(GTK_FILE_CHOOSER(w),
                                                client->gconf);

        if (showOptions) {
            TrgTorrentAddDialog *dialog =
                trg_torrent_add_dialog_new(win, client, l);

            gtk_widget_show_all(GTK_WIDGET(dialog));
        } else {
            struct add_torrent_threadfunc_args *args =
                g_new(struct add_torrent_threadfunc_args, 1);
            args->list = l;
            args->cb_data = win;
            args->client = client;
            args->paused = pref_get_start_paused(client->gconf);
            args->extraArgs = FALSE;

            launch_add_thread(args);
        }
    }

    gtk_widget_destroy(GTK_WIDGET(w));
}