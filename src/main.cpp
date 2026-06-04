#include <gtk/gtk.h>
#include <gdal.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "chart_loader.hpp"
#include "chart_widget.hpp"
#include "settings.hpp"

// Shared application state, passed to GTK callbacks as user_data.
struct App {
    GtkApplication* app = nullptr;
    GtkWindow*      win = nullptr;
    ChartWidget*    chart = nullptr;
    GtkLabel*       statusLeft = nullptr;   // folder + counts
    GtkLabel*       statusRight = nullptr;  // cursor lat/lon
    std::string     dir;
};

static void set_status_left(App* a, const std::string& extra) {
    std::string s = a->dir.empty() ? "No chart folder selected" : a->dir;
    if (!extra.empty()) s += "      " + extra;
    gtk_label_set_text(a->statusLeft, s.c_str());
}

static void show_error(App* a, const std::string& msg) {
    GtkAlertDialog* d = gtk_alert_dialog_new("%s", msg.c_str());
    gtk_alert_dialog_show(d, a->win);
    g_object_unref(d);
}

// NOTE: loading is synchronous here. For very large chart sets this briefly
// blocks the UI; moving ChartSet::loadDirectory onto a worker thread (GTask)
// and marshalling the result back with g_idle_add is the natural next step.
static void load_folder(App* a, const std::string& dir) {
    auto cs = std::make_shared<ChartSet>();
    std::string err;
    if (cs->loadDirectory(dir, err)) {
        a->dir = dir;
        a->chart->setChart(cs);
        settings_save_dir(dir);
        char buf[160];
        std::snprintf(buf, sizeof buf, "%zu cell(s)  ·  %zu feature(s)",
                      cs->cellCount(), cs->features().size());
        set_status_left(a, buf);
    } else {
        show_error(a, err);
    }
}

static void on_folder_chosen(GObject* src, GAsyncResult* res, gpointer data) {
    App* a = static_cast<App*>(data);
    GError* err = nullptr;
    GFile* file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, &err);
    if (file) {
        char* path = g_file_get_path(file);
        if (path) { load_folder(a, path); g_free(path); }
        g_object_unref(file);
    }
    if (err) g_error_free(err); // user cancelled or genuine error; nothing to do
}

static void on_open_clicked(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    GtkFileDialog* dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Select ENC Chart Folder");
    gtk_file_dialog_select_folder(dlg, a->win, nullptr, on_folder_chosen, a);
    g_object_unref(dlg);
}

static void on_fit_clicked(GtkButton*, gpointer data) {
    static_cast<App*>(data)->chart->resetView();
}

static void on_motion(double lon, double lat, void* ud) {
    App* a = static_cast<App*>(ud);
    char buf[80];
    std::snprintf(buf, sizeof buf, "%.4f\xc2\xb0%c   %.4f\xc2\xb0%c",
                  std::fabs(lat), lat >= 0 ? 'N' : 'S',
                  std::fabs(lon), lon >= 0 ? 'E' : 'W');
    gtk_label_set_text(a->statusRight, buf);
}

static void activate(GtkApplication* app, gpointer data) {
    App* a = static_cast<App*>(data);
    a->app = app;

    GtkWidget* win = gtk_application_window_new(app);
    a->win = GTK_WINDOW(win);
    gtk_window_set_title(a->win, "Marine Chart Viewer");
    gtk_window_set_default_size(a->win, 1100, 750);

    GtkWidget* header = gtk_header_bar_new();
    gtk_window_set_titlebar(a->win, header);

    GtkWidget* openBtn = gtk_button_new_with_label("Open Chart Folder");
    g_signal_connect(openBtn, "clicked", G_CALLBACK(on_open_clicked), a);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), openBtn);

    GtkWidget* fitBtn = gtk_button_new_with_label("Fit");
    g_signal_connect(fitBtn, "clicked", G_CALLBACK(on_fit_clicked), a);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), fitBtn);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    a->chart = new ChartWidget();
    a->chart->setMotionCallback(on_motion, a);
    gtk_box_append(GTK_BOX(box), a->chart->widget());

    // Status bar.
    GtkWidget* sbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(sbar, 8);
    gtk_widget_set_margin_end(sbar, 8);
    gtk_widget_set_margin_top(sbar, 4);
    gtk_widget_set_margin_bottom(sbar, 4);

    a->statusLeft = GTK_LABEL(gtk_label_new("No chart folder selected"));
    gtk_label_set_xalign(a->statusLeft, 0.0);
    gtk_label_set_ellipsize(a->statusLeft, PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(GTK_WIDGET(a->statusLeft), TRUE);

    a->statusRight = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(a->statusRight, 1.0);

    gtk_box_append(GTK_BOX(sbar), GTK_WIDGET(a->statusLeft));
    gtk_box_append(GTK_BOX(sbar), GTK_WIDGET(a->statusRight));
    gtk_box_append(GTK_BOX(box), sbar);

    gtk_window_set_child(a->win, box);
    gtk_window_present(a->win);

    // Reopen the last-used folder, if it still exists.
    std::string saved = settings_load_dir();
    if (!saved.empty() && g_file_test(saved.c_str(), G_FILE_TEST_IS_DIR))
        load_folder(a, saved);
}

int main(int argc, char** argv) {
    GDALAllRegister();

    App a;
    GtkApplication* app =
        gtk_application_new("org.example.marinechartviewer", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &a);
    int status = g_application_run(G_APPLICATION(app), argc, argv);

    delete a.chart;
    g_object_unref(app);
    return status;
}
