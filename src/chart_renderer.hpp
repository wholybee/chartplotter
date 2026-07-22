#pragma once
// src/chart_renderer.hpp
//
// Stage 6: the chart-renderer seam (renderer architecture plan, Layer 7).
//
// IChartRenderer is the contract between the ChartView *shell* (camera, touch
// input, follow-ownship, settings, status signals, dynamic overlays) and a
// chart-cell *renderer* backend. It lets a future retained GPU backend
// (PreparedGpuChartRenderer, Stage 7) slot in without the shell, the overlays,
// or the plugins knowing which backend is active.
//
// Today there is one backend: ChartView itself is the painter-backed renderer
// (it implements this interface and draws cells with QPainter). The interface is
// deliberately small and renderer-neutral — it carries a camera, bundled display
// settings, repaint requests, picking, and overlay registration, all in value
// types rather than QPainter specifics.

#include <QList>
#include <QPointF>
#include "chart_object.hpp"   // ChartObjectInfo

class QWidget;
class IChartOverlay;

// Camera: where the view is and how far it is zoomed. lon/lat are the view
// centre in degrees; scale is the renderer's zoom (pixels per projected metre,
// matching ChartView's currentView/restoreView). Geographic centre keeps the
// camera backend-neutral (no scene-frame assumptions leak across the seam).
struct ChartCamera {
    double lon   = 0.0;
    double lat   = 0.0;
    double scale = 0.0;
};

// Bundled chart-display state the shell pushes to the renderer. These are the
// "what to draw" toggles/biases that change rendering output; orthogonal
// concerns (unit labels, basemap folder, raster folders) keep their own setters.
struct ChartDisplaySettings {
    bool   showSoundings     = true;
    bool   showSymbols       = true;
    bool   showText          = true;
    bool   showDepthContours = true;
    bool   showRasterCharts  = true;
    bool   vectorOverlay     = false;
    double chartDetailLevel  = 0.0;   // -2..+2 band bias
    double scaminLevel       = 0.0;   // -1..+1 SCAMIN declutter bias
    double symbolScale       = 1.0;   // 0.5..3.0
    double textScale         = 1.0;   // 0.5..3.0, label size multiplier
    double soundingScale     = 1.0;   // 0.5..3.0, sounding size multiplier
    bool   labelNudge        = true;  // nudge labels to reduce overlap
    double labelNudgeMaxPx   = 20.0;  // max label nudge distance (device px)
};

// Why a repaint was requested. Coalesced batches data-driven updates (GPS/AIS/
// route/plugin) into one frame; Immediate paints now (interactive pan/zoom).
enum class RepaintReason { Coalesced, Immediate };

// Result of a pick: the chart objects under a screen point, nearest/most-
// specific first (empty when the click hit blank chart).
struct PickResult {
    QList<ChartObjectInfo> objects;
    bool empty() const { return objects.isEmpty(); }
};

// The chart-cell renderer behind ChartView. A backend owns how cells are loaded,
// retained, and drawn; the shell drives it through this contract.
class IChartRenderer {
public:
    virtual ~IChartRenderer() = default;

    // The QWidget the cells are drawn on (so the shell can embed/size it).
    virtual QWidget* widget() = 0;

    // Camera (view centre + zoom).
    virtual void        setCamera(const ChartCamera& camera) = 0;
    virtual ChartCamera camera() const = 0;

    // Push the chart-display configuration.
    virtual void setDisplaySettings(const ChartDisplaySettings& settings) = 0;

    // Ask for a repaint; Coalesced batches data-driven updates.
    virtual void requestRepaint(RepaintReason reason) = 0;

    // Identify chart objects under a screen point (CPU geometry pick; does not
    // depend on rendered pixels).
    virtual PickResult pick(const QPointF& screenPos) = 0;

    // Dynamic application/plugin overlays, drawn on top each frame in
    // registration order. Backend-agnostic: an overlay never learns which
    // renderer is active. Not owned.
    virtual void addOverlay(IChartOverlay* overlay) = 0;
    virtual void removeOverlay(IChartOverlay* overlay) = 0;
};
