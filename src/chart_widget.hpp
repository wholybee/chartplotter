#pragma once
#include <gtk/gtk.h>
#include <memory>
#include "chart_loader.hpp"

// Wraps a GtkDrawingArea and owns the viewport (pan / zoom) state plus all of
// the rendering and input handling. The widget itself is a normal GtkWidget*
// retrieved via widget().
class ChartWidget {
public:
    ChartWidget();

    GtkWidget* widget() const { return area_; }

    // Replace the displayed chart set and re-fit the view.
    void setChart(std::shared_ptr<ChartSet> chart);

    // Re-fit the current chart to the window.
    void resetView();

    // Called on pointer motion with the geographic coordinate under the cursor.
    using MotionFn = void (*)(double lon, double lat, void* userData);
    void setMotionCallback(MotionFn cb, void* userData) {
        motionCb_ = cb;
        motionUd_ = userData;
    }

private:
    void fitToBounds(int w, int h);
    void draw(cairo_t* cr, int w, int h);

    // GTK C-callback trampolines.
    static void s_draw(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer self);
    static void s_dragBegin(GtkGestureDrag*, double, double, gpointer self);
    static void s_dragUpdate(GtkGestureDrag*, double ox, double oy, gpointer self);
    static gboolean s_scroll(GtkEventControllerScroll*, double dx, double dy, gpointer self);
    static void s_motion(GtkEventControllerMotion*, double x, double y, gpointer self);

    GtkWidget* area_ = nullptr;
    std::shared_ptr<ChartSet> chart_;

    // Viewport: world coordinate at screen centre, and pixels-per-metre scale.
    double centerX_ = 0.0;
    double centerY_ = 0.0;
    double scale_   = 1.0;
    bool   needFit_ = true;

    double mouseX_ = 0.0, mouseY_ = 0.0;        // last pointer position (px)
    double dragStartCx_ = 0.0, dragStartCy_ = 0.0;

    MotionFn motionCb_ = nullptr;
    void*    motionUd_ = nullptr;
};
