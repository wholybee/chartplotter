#pragma once
#include <functional>
#include "plugin_api.hpp"     // IChartOverlay, ChartViewport
#include "chart_view.hpp"     // IChartEditor
#include "route_types.hpp"

class RouteStore;
class NavDataStore;
class Settings;

// Identifies a saved route or waypoint that the user tapped on the chart. Used
// by the click callback so the host (MainWindow) can pop a quick-info window.
struct ClickedRouteObject {
    enum class Kind { Waypoint, Route };
    Kind    kind  = Kind::Waypoint;
    qint64  id    = -1;
    QPointF screenPt;          // where the tap landed, for popup anchoring
    // For a Route hit: index of the route point nearest the tap — the clicked
    // node, or the end (destination) node of the clicked leg. -1 when N/A (a
    // standalone-waypoint hit). Lets "Navigate" begin at the tapped waypoint.
    int     pointIndex = -1;
};

// Draws saved routes and waypoints on the chart, and hosts the in-place editor
// used to create/edit them. It is both an IChartOverlay (paint + tap pick) and an
// IChartEditor (press/move/release for node dragging), so one object owns all the
// route drawing and interaction.
//
// Interaction split (see ChartView's mouse handling):
//   - Press on a working-route node  -> onPress() claims the gesture; a drag
//     repositions the node, a tap (no movement) selects it.
//   - Tap on empty chart             -> hitTest() appends a point (route modes)
//     or places the waypoint (CreateWaypoint mode).
// Outside an edit mode hitTest() declines, so AIS/other picks keep working.
class RouteOverlay : public IChartOverlay, public IChartEditor {
public:
    enum class Mode { None, CreateRoute, EditRoute, CreateWaypoint, EditWaypoint };

    explicit RouteOverlay(const RouteStore* store) : store_(store) {}

    // Optional read-only nav source: when a route is being navigated, the active
    // (next) waypoint from its NavigationData is highlighted with a red ring. Also
    // supplies the magnetic variation used to show leg headings in magnetic mode.
    void setNavSource(const NavDataStore* nav) { nav_ = nav; }

    // Optional units source: the distance unit and true/magnetic bearing
    // preference used to label each route leg with its distance and heading.
    // Without it, labels default to nautical miles and true bearings.
    void setUnitsSource(const Settings* settings) { settings_ = settings; }

    // Callbacks into the host (MainWindow). All optional.
    void setRepaintCallback(std::function<void()> cb) { repaint_ = std::move(cb); }
    // Invoked when the user taps to place a waypoint (CreateWaypoint mode).
    void setWaypointPlacedCallback(std::function<void(double lat, double lon)> cb) {
        onWaypointPlaced_ = std::move(cb);
    }
    // Invoked when the selected-node state changes (drives the Delete button).
    void setSelectionChangedCallback(std::function<void(bool hasSelection)> cb) {
        onSelectionChanged_ = std::move(cb);
    }
    // Invoked when the user taps a saved route or waypoint outside an edit
    // session (the chart-quick-info entry point — mirrors AisOverlay's pattern).
    void setObjectClickedCallback(std::function<void(const ClickedRouteObject&)> cb) {
        onObjectClicked_ = std::move(cb);
    }

    // IChartOverlay ----------------------------------------------------------
    void paint(QPainter& painter, const ChartViewport& viewport) override;
    bool hitTest(const QPointF& screenPt) override;

    // IChartEditor -----------------------------------------------------------
    bool onPress(const QPointF& screenPt) override;
    void onMove(const QPointF& screenPt) override;
    void onRelease(const QPointF& screenPt) override;

    // Edit session control (driven by MainWindow) ----------------------------
    void beginCreateRoute();
    void beginEditRoute(const Route& r);   // working copy keeps r.id
    void beginCreateWaypoint();
    void beginEditWaypoint(const Waypoint& w);   // working copy keeps w.id
    void endEditing();
    Mode mode() const { return mode_; }

    const Route&    workingRoute()    const { return work_; }     // read on Complete Route
    const Waypoint& workingWaypoint() const { return editWpt_; }  // read on Complete (wpt)
    bool hasSelectedNode() const { return selected_ >= 0; }
    void deleteSelectedNode();

private:
    int  nodeAt(const QPointF& screenPt) const;   // index in work_.points, or -1
    void notifySelection();
    void repaint();
    // Local magnetic variation (easterly positive) from the nav source, for
    // converting leg headings to magnetic. 0 when unavailable.
    double magneticVariation() const;

    const RouteStore*   store_ = nullptr;
    const NavDataStore* nav_   = nullptr;   // for the active-waypoint highlight
    const Settings*     settings_ = nullptr;   // for leg distance/heading units
    ChartViewport     vp_;
    bool              haveVp_ = false;

    Mode mode_ = Mode::None;
    Route work_;                 // the route being created/edited
    Waypoint editWpt_;           // the waypoint being edited (EditWaypoint mode)
    int  selected_ = -1;         // selected node index (-1 none)
    int  dragging_ = -1;         // node being dragged (-1 none)
    bool dragMoved_ = false;     // distinguishes a drag from a tap-select
    QPointF pressPos_;

    std::function<void()>                     repaint_;
    std::function<void(double, double)>       onWaypointPlaced_;
    std::function<void(bool)>                 onSelectionChanged_;
    std::function<void(const ClickedRouteObject&)> onObjectClicked_;
};
