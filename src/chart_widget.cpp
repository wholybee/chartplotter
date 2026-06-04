#include "chart_widget.hpp"
#include "projection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

// Fill colour for area features. Depth areas use a traditional shallow->deep
// ramp (darker blue = shallower / more hazardous, near-white = deep water).
void fillColor(const Feature& f, double& r, double& g, double& b) {
    if (f.kind == FeatureKind::LandArea) { r = 0.85; g = 0.78; b = 0.58; return; }

    if (!f.hasDepth) { r = 0.72; g = 0.83; b = 0.92; return; }
    double d = f.depth;
    if      (d <  0.0) { r = 0.62; g = 0.74; b = 0.55; } // drying / above datum
    else if (d <  2.0) { r = 0.40; g = 0.66; b = 0.85; }
    else if (d <  5.0) { r = 0.56; g = 0.76; b = 0.89; }
    else if (d < 10.0) { r = 0.72; g = 0.85; b = 0.94; }
    else if (d < 20.0) { r = 0.85; g = 0.92; b = 0.98; }
    else               { r = 0.95; g = 0.97; b = 1.00; }
}

} // namespace

ChartWidget::ChartWidget() {
    area_ = gtk_drawing_area_new();
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_widget_set_vexpand(area_, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_),
                                   &ChartWidget::s_draw, this, nullptr);

    GtkGesture* drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin",  G_CALLBACK(&ChartWidget::s_dragBegin),  this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(&ChartWidget::s_dragUpdate), this);
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(drag));

    GtkEventController* scroll =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(&ChartWidget::s_scroll), this);
    gtk_widget_add_controller(area_, scroll);

    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(&ChartWidget::s_motion), this);
    gtk_widget_add_controller(area_, motion);
}

void ChartWidget::setChart(std::shared_ptr<ChartSet> chart) {
    chart_ = std::move(chart);
    needFit_ = true;
    gtk_widget_queue_draw(area_);
}

void ChartWidget::resetView() {
    needFit_ = true;
    gtk_widget_queue_draw(area_);
}

void ChartWidget::fitToBounds(int w, int h) {
    if (!chart_ || !chart_->bounds().valid() || w <= 0 || h <= 0) return;
    const BBox& b = chart_->bounds();
    centerX_ = (b.minx + b.maxx) / 2.0;
    centerY_ = (b.miny + b.maxy) / 2.0;
    double dx = std::max(b.maxx - b.minx, 1.0);
    double dy = std::max(b.maxy - b.miny, 1.0);
    scale_ = std::min(w / dx, h / dy) * 0.92;
    if (scale_ <= 0.0) scale_ = 1e-4;
    needFit_ = false;
}

// --- rendering -------------------------------------------------------------

void ChartWidget::draw(cairo_t* cr, int w, int h) {
    cairo_set_source_rgb(cr, 0.80, 0.88, 0.95); // backdrop sea colour
    cairo_paint(cr);

    if (!chart_) {
        cairo_set_source_rgb(cr, 0.30, 0.30, 0.30);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 16);
        const char* msg = "Open a chart folder to begin (toolbar button).";
        cairo_text_extents_t ext;
        cairo_text_extents(cr, msg, &ext);
        cairo_move_to(cr, w / 2.0 - ext.width / 2.0, h / 2.0);
        cairo_show_text(cr, msg);
        return;
    }

    if (needFit_) fitToBounds(w, h);

    const double sc = scale_;
    const double cx = centerX_;
    const double cy = centerY_;
    auto sx = [&](double X) { return (X - cx) * sc + w / 2.0; };
    auto sy = [&](double Y) { return h / 2.0 - (Y - cy) * sc; };

    BBox vis;
    vis.minx = cx - (w / 2.0) / sc;
    vis.maxx = cx + (w / 2.0) / sc;
    vis.miny = cy - (h / 2.0) / sc;
    vis.maxy = cy + (h / 2.0) / sc;

    // Suppress point clutter (soundings, buoys) until reasonably zoomed in.
    const double visWidthMeters = w / sc;
    const bool showPoints = visWidthMeters < 20000.0;

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    for (const Feature& f : chart_->features()) {
        if (!f.bbox.intersects(vis)) continue;

        switch (f.kind) {
            case FeatureKind::DepthArea:
            case FeatureKind::LandArea: {
                double r, g, b;
                fillColor(f, r, g, b);
                cairo_new_path(cr);
                cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
                for (const auto& ring : f.rings) {
                    if (ring.size() < 2) continue;
                    cairo_move_to(cr, sx(ring[0].x), sy(ring[0].y));
                    for (std::size_t i = 1; i < ring.size(); ++i)
                        cairo_line_to(cr, sx(ring[i].x), sy(ring[i].y));
                    cairo_close_path(cr);
                }
                cairo_set_source_rgb(cr, r, g, b);
                if (f.kind == FeatureKind::LandArea) {
                    cairo_fill_preserve(cr);
                    cairo_set_source_rgb(cr, 0.45, 0.38, 0.25);
                    cairo_set_line_width(cr, 1.0);
                    cairo_stroke(cr);
                } else {
                    cairo_fill(cr);
                }
                break;
            }

            case FeatureKind::OtherArea:
            case FeatureKind::DepthContour:
            case FeatureKind::Coastline:
            case FeatureKind::OtherLine: {
                if (f.kind == FeatureKind::Coastline) {
                    cairo_set_source_rgb(cr, 0.25, 0.20, 0.12);
                    cairo_set_line_width(cr, 1.4);
                } else if (f.kind == FeatureKind::DepthContour) {
                    cairo_set_source_rgb(cr, 0.45, 0.60, 0.78);
                    cairo_set_line_width(cr, 0.8);
                } else if (f.kind == FeatureKind::OtherArea) {
                    cairo_set_source_rgba(cr, 0.40, 0.40, 0.45, 0.6);
                    cairo_set_line_width(cr, 0.7);
                } else {
                    cairo_set_source_rgb(cr, 0.40, 0.40, 0.50);
                    cairo_set_line_width(cr, 0.8);
                }
                for (const auto& ring : f.rings) {
                    if (ring.size() < 2) continue;
                    cairo_new_path(cr);
                    cairo_move_to(cr, sx(ring[0].x), sy(ring[0].y));
                    for (std::size_t i = 1; i < ring.size(); ++i)
                        cairo_line_to(cr, sx(ring[i].x), sy(ring[i].y));
                    cairo_stroke(cr);
                }
                break;
            }

            case FeatureKind::Sounding: {
                if (!showPoints || f.rings.empty() || f.rings[0].empty()) break;
                double X = sx(f.rings[0][0].x);
                double Y = sy(f.rings[0][0].y);
                cairo_set_source_rgb(cr, 0.10, 0.20, 0.45);
                cairo_set_font_size(cr, 11);
                char buf[32];
                if (f.hasDepth) {
                    if (f.depth < 10.0) std::snprintf(buf, sizeof buf, "%.1f", f.depth);
                    else                std::snprintf(buf, sizeof buf, "%.0f", f.depth);
                } else {
                    std::snprintf(buf, sizeof buf, "%s", ".");
                }
                cairo_move_to(cr, X + 2.0, Y + 4.0);
                cairo_show_text(cr, buf);
                break;
            }

            case FeatureKind::Point: {
                if (!showPoints || f.rings.empty() || f.rings[0].empty()) break;
                double X = sx(f.rings[0][0].x);
                double Y = sy(f.rings[0][0].y);
                cairo_new_path(cr);
                cairo_set_source_rgb(cr, 0.70, 0.10, 0.50);
                cairo_arc(cr, X, Y, 3.0, 0.0, 2.0 * proj::PI);
                cairo_fill(cr);
                break;
            }
        }
    }
}

// --- input -----------------------------------------------------------------

void ChartWidget::s_draw(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer self) {
    static_cast<ChartWidget*>(self)->draw(cr, w, h);
}

void ChartWidget::s_dragBegin(GtkGestureDrag*, double, double, gpointer selfp) {
    auto* self = static_cast<ChartWidget*>(selfp);
    self->dragStartCx_ = self->centerX_;
    self->dragStartCy_ = self->centerY_;
}

void ChartWidget::s_dragUpdate(GtkGestureDrag*, double ox, double oy, gpointer selfp) {
    auto* self = static_cast<ChartWidget*>(selfp);
    if (self->scale_ <= 0.0) return;
    self->centerX_ = self->dragStartCx_ - ox / self->scale_;
    self->centerY_ = self->dragStartCy_ + oy / self->scale_; // screen Y is flipped
    gtk_widget_queue_draw(self->area_);
}

gboolean ChartWidget::s_scroll(GtkEventControllerScroll*, double, double dy, gpointer selfp) {
    auto* self = static_cast<ChartWidget*>(selfp);
    int w = gtk_widget_get_width(self->area_);
    int h = gtk_widget_get_height(self->area_);
    double sc = self->scale_;
    if (sc <= 0.0) return TRUE;

    // World coordinate currently under the cursor (kept fixed while zooming).
    double wx = self->centerX_ + (self->mouseX_ - w / 2.0) / sc;
    double wy = self->centerY_ - (self->mouseY_ - h / 2.0) / sc;

    double factor = (dy < 0.0) ? 1.15 : (1.0 / 1.15); // wheel up = zoom in
    sc *= factor;
    sc = std::clamp(sc, 1e-8, 1e3);
    self->scale_ = sc;

    self->centerX_ = wx - (self->mouseX_ - w / 2.0) / sc;
    self->centerY_ = wy + (self->mouseY_ - h / 2.0) / sc;
    gtk_widget_queue_draw(self->area_);
    return TRUE;
}

void ChartWidget::s_motion(GtkEventControllerMotion*, double x, double y, gpointer selfp) {
    auto* self = static_cast<ChartWidget*>(selfp);
    self->mouseX_ = x;
    self->mouseY_ = y;
    if (self->motionCb_ && self->chart_) {
        int w = gtk_widget_get_width(self->area_);
        int h = gtk_widget_get_height(self->area_);
        double sc = self->scale_;
        if (sc <= 0.0) return;
        double wx = self->centerX_ + (x - w / 2.0) / sc;
        double wy = self->centerY_ - (y - h / 2.0) / sc;
        self->motionCb_(proj::xToLon(wx), proj::yToLat(wy), self->motionUd_);
    }
}
