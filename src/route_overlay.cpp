#include "route_overlay.hpp"
#include "route_store.hpp"
#include "route_colors.hpp"
#include "nav_data_store.hpp"
#include "settings.hpp"
#include "units.hpp"
#include "geo_nav.hpp"

#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QFont>
#include <QFontMetrics>
#include <QPainterPath>
#include <algorithm>
#include <vector>
#include <cmath>

namespace {
// Device-pixel sizes / colours. Routes are violet; waypoints orange; the route
// currently being edited is drawn brighter (yellow) so it stands out from saved
// ones, with the selected node ringed in red.
constexpr double kNodeRadius   = 5.0;
constexpr double kEditNode     = 6.0;
constexpr double kPickRadiusPx = 18.0;
constexpr double kActiveRing   = 11.0;   // red highlight around the next waypoint
// A leg must be at least this long on screen to be worth labelling; shorter legs
// (e.g. a many-point route seen zoomed out) would just pile labels on top of each
// other, so they are left unlabelled until the user zooms in.
constexpr double kMinLegLabelPx = 44.0;
constexpr double kLegLabelOffset = 12.0;   // px the label sits off the leg line
const QColor kRouteLine   (0xB0, 0x30, 0xD0);   // default; per-route DisplayColor overrides
const QColor kWptFill      (0xFF, 0x8C, 0x00);
const QColor kWptEdge      (0x40, 0x20, 0x00);
const QColor kEditLine     (0xF0, 0xC0, 0x00);
const QColor kEditNodeFill (0xFF, 0xF0, 0x60);
const QColor kSelRing       (0xE0, 0x20, 0x20);
const QColor kLabelBg      (0, 0, 0, 150);
const QColor kLabelFg      (255, 255, 255);
// Tracks: teal, well clear of the violet routes and orange waypoints. The track
// being recorded is drawn brighter and a touch thicker so the live trail reads
// apart from the finished ones.
const QColor kTrackLine     (0x00, 0x86, 0x8C);
const QColor kTrackLiveLine (0x18, 0xC8, 0xC0);
constexpr double kTrackWidth     = 2.0;
constexpr double kTrackLiveWidth = 2.6;

bool onScreen(const QPointF& p, const QRectF& r) { return r.contains(p); }

// Draw a short label with a translucent rounded background for legibility over
// busy chart colours.
void drawLabel(QPainter& p, const QPointF& at, const QString& text) {
    if (text.isEmpty()) return;
    const QFontMetrics fm(p.font());
    const QRectF tr = fm.boundingRect(text);
    const QRectF bg(at.x(), at.y() - tr.height() / 2.0 - 2.0,
                    tr.width() + 8.0, tr.height() + 4.0);
    p.setPen(Qt::NoPen);
    p.setBrush(kLabelBg);
    p.drawRoundedRect(bg, 3.0, 3.0);
    p.setPen(kLabelFg);
    p.drawText(bg.adjusted(4.0, 0, -4.0, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
}

// Like drawLabel but centred on `at`, and optionally rotated by `angleDeg`
// (clockwise, screen space) so the text runs along a route leg. Used for leg
// labels sitting at a midpoint.
void drawCenteredLabel(QPainter& p, const QPointF& at, const QString& text,
                       double angleDeg = 0.0) {
    if (text.isEmpty()) return;
    const QFontMetrics fm(p.font());
    const QRectF tr = fm.boundingRect(text);
    const double w = tr.width() + 8.0, h = tr.height() + 4.0;
    p.save();
    p.translate(at);
    p.rotate(angleDeg);
    const QRectF bg(-w / 2.0, -h / 2.0, w, h);   // centred on the rotation origin
    p.setPen(Qt::NoPen);
    p.setBrush(kLabelBg);
    p.drawRoundedRect(bg, 3.0, 3.0);
    p.setPen(kLabelFg);
    p.drawText(bg, Qt::AlignCenter, text);
    p.restore();
}

// Label one route leg (a -> b) with its distance and compass heading, in the
// user's chosen units, at the leg's on-screen midpoint. `sa`/`sb` are the leg's
// screen endpoints; `a`/`b` the geographic ones (distance/bearing are computed on
// the sphere, not from pixels). Skipped when the leg is too short on screen or its
// midpoint falls outside the cull rect.
void drawLegLabel(QPainter& p, const RoutePoint& a, const RoutePoint& b,
                  const QPointF& sa, const QPointF& sb,
                  DistanceUnit distUnit, BearingMode brgMode, double variationDeg,
                  const QRectF& cull) {
    const QPointF d = sb - sa;
    const double lenPx = std::hypot(d.x(), d.y());
    if (lenPx < kMinLegLabelPx) return;
    const QPointF mid = (sa + sb) * 0.5;
    if (!cull.contains(mid)) return;

    const double distNm = geonav::distanceNm(a.lat, a.lon, b.lat, b.lon);
    double brg = geonav::initialBearingDeg(a.lat, a.lon, b.lat, b.lon);
    if (brgMode == BearingMode::Magnetic)
        brg = geonav::magneticFromTrue(brg, variationDeg);

    const QString text = units::formatDistance(distNm, distUnit)
                       + QStringLiteral("   ") + units::formatBearing(brg, brgMode);

    // Run the text along the leg, but flip any leg pointing "backwards" by 180°
    // so the label is never upside down.
    double angleDeg = std::atan2(d.y(), d.x()) * geonav::R2D;
    if (angleDeg > 90.0)       angleDeg -= 180.0;
    else if (angleDeg < -90.0) angleDeg += 180.0;

    // Offset perpendicular to the leg so the label sits just off the line; keep it
    // on the upper side on screen regardless of the leg's direction.
    QPointF perp = QPointF(-d.y(), d.x()) / lenPx;
    if (perp.y() > 0.0) perp = -perp;
    drawCenteredLabel(p, mid + perp * kLegLabelOffset, text, angleDeg);
}
}  // namespace

void RouteOverlay::repaint() { if (repaint_) repaint_(); }

void RouteOverlay::notifySelection() {
    if (onSelectionChanged_) onSelectionChanged_(hasSelectedNode());
}

// Local magnetic variation for magnetic-mode leg headings. Prefer a published
// variation; otherwise derive it from true vs magnetic heading if both are
// present. Mirrors RouteNavigator's approach so on-chart and navigator headings
// agree. Falls back to 0 (magnetic == true) when nothing is available.
double RouteOverlay::magneticVariation() const {
    if (!nav_) return 0.0;
    const OwnshipState& os = nav_->ownship();
    if (os.variationDeg.valid()) return os.variationDeg.value;
    if (os.headingDegTrue.valid() && os.headingDegMag.valid())
        return os.headingDegTrue.value - os.headingDegMag.value;
    return 0.0;
}

// ---- drawing ---------------------------------------------------------------

// Recorded tracks: one polyline each, no per-point markers (a long recording has
// thousands of points, and drawing a node at each would be a solid smear). Points
// closer than a pixel to their predecessor are dropped, so a boat that lingers
// contributes one vertex instead of hundreds and the cost of a long track scales
// with its on-screen length rather than its duration.
void RouteOverlay::paintTracks(QPainter& p, const ChartViewport& vp,
                               const QRectF& cull) const {
    if (!store_) return;
    constexpr double kMinVertexStepSq = 1.0;   // px^2 between kept vertices

    for (const Track& t : store_->tracks()) {
        if (!t.visible || t.points.size() < 2) continue;

        std::vector<QPointF> pts;
        pts.reserve(t.points.size());
        double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
        for (const TrackPoint& tp : t.points) {
            const QPointF s = vp.geoToScreen(tp.lat, tp.lon);
            minX = std::min(minX, s.x()); maxX = std::max(maxX, s.x());
            minY = std::min(minY, s.y()); maxY = std::max(maxY, s.y());
            if (!pts.empty()) {
                const QPointF d = s - pts.back();
                if (d.x() * d.x() + d.y() * d.y() < kMinVertexStepSq) continue;
            }
            pts.push_back(s);
        }
        if (!QRectF(QPointF(minX, minY), QPointF(maxX, maxY)).intersects(cull))
            continue;   // whole track off-screen
        if (pts.size() < 2) continue;   // collapsed to a single pixel

        const bool live = (t.id == recordingTrackId_);
        p.setPen(QPen(live ? kTrackLiveLine : kTrackLine,
                      live ? kTrackLiveWidth : kTrackWidth));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(pts.data(), int(pts.size()));

        if (!t.name.isEmpty() && onScreen(pts.front(), cull))
            drawLabel(p, pts.front() + QPointF(6.0, 0), t.name);
    }
}

void RouteOverlay::paint(QPainter& p, const ChartViewport& vp) {
    vp_ = vp;
    haveVp_ = true;

    const QRectF screen(QPointF(0, 0), QPointF(vp.size.width(), vp.size.height()));
    const QRectF cull = screen.adjusted(-64, -64, 64, 64);   // margin for labels/legs
    p.setRenderHint(QPainter::Antialiasing, true);

    QFont f = p.font();
    f.setPointSizeF(9.0);
    p.setFont(f);

    // Unit preferences for the per-leg distance/heading labels. Read once per
    // frame; default to nm + true when no settings source is wired.
    const DistanceUnit distUnit = settings_ ? settings_->distanceUnit()
                                            : DistanceUnit::NauticalMiles;
    const BearingMode  brgMode  = settings_ ? settings_->bearingMode()
                                            : BearingMode::True;
    const double variation = magneticVariation();

    // Recorded tracks -------------------------------------------------------
    // First, so routes and waypoints draw over the history rather than under it.
    paintTracks(p, vp, cull);

    // Saved routes ----------------------------------------------------------
    if (store_) {
        for (const Route& r : store_->routes()) {
            if (!r.visible || r.points.isEmpty()) continue;
            // Hide the saved copy of the route currently being edited; the bright
            // working copy is drawn on top instead.
            if (mode_ == Mode::EditRoute && r.id == work_.id) continue;
            std::vector<QPointF> pts;
            pts.reserve(r.points.size());
            double minX = 1e30, minY = 1e30, maxX = -1e30, maxY = -1e30;
            for (const RoutePoint& rp : r.points) {
                const QPointF s = vp.geoToScreen(rp.lat, rp.lon);
                pts.push_back(s);
                minX = std::min(minX, s.x()); maxX = std::max(maxX, s.x());
                minY = std::min(minY, s.y()); maxY = std::max(maxY, s.y());
            }
            const QRectF rb(QPointF(minX, minY), QPointF(maxX, maxY));
            if (!rb.intersects(cull)) continue;   // whole route off-screen

            // Per-route display colour (from GPX <gpxx:DisplayColor>); falls back
            // to the default violet. Nodes use a slightly darker shade for contrast.
            const QColor lineColor = routecolor::toColor(r.displayColor, kRouteLine);
            const QColor nodeColor = lineColor.darker(130);
            p.setPen(QPen(lineColor, 2.0));
            p.setBrush(Qt::NoBrush);
            for (size_t i = 1; i < pts.size(); ++i) p.drawLine(pts[i - 1], pts[i]);

            // Distance + heading of each leg, in the user's chosen units.
            for (size_t i = 1; i < pts.size(); ++i)
                drawLegLabel(p, r.points[int(i) - 1], r.points[int(i)],
                             pts[i - 1], pts[i], distUnit, brgMode, variation, cull);

            p.setPen(QPen(QColor(255, 255, 255), 1.0));
            p.setBrush(nodeColor);
            for (const QPointF& s : pts) {
                if (!onScreen(s, cull)) continue;
                p.drawEllipse(s, kNodeRadius, kNodeRadius);
            }
            if (!r.name.isEmpty() && onScreen(pts.front(), cull))
                drawLabel(p, pts.front() + QPointF(kNodeRadius + 4, 0), r.name);
        }

        // Saved waypoints ---------------------------------------------------
        for (const Waypoint& w : store_->waypoints()) {
            if (!w.visible) continue;
            // Hide the saved copy of the waypoint currently being edited; the
            // bright draggable copy is drawn below instead.
            if (mode_ == Mode::EditWaypoint && w.id == editWpt_.id) continue;
            const QPointF s = vp.geoToScreen(w.lat, w.lon);
            if (!onScreen(s, cull)) continue;
            p.setPen(QPen(kWptEdge, 1.5));
            p.setBrush(kWptFill);
            // Diamond marker.
            QPainterPath dia;
            const double a = 6.0;
            dia.moveTo(s + QPointF(0, -a));
            dia.lineTo(s + QPointF(a, 0));
            dia.lineTo(s + QPointF(0, a));
            dia.lineTo(s + QPointF(-a, 0));
            dia.closeSubpath();
            p.drawPath(dia);
            if (!w.name.isEmpty())
                drawLabel(p, s + QPointF(a + 4, 0), w.name);
        }
    }

    // In-progress route (create/edit) ---------------------------------------
    if (mode_ == Mode::CreateRoute || mode_ == Mode::EditRoute) {
        std::vector<QPointF> pts;
        pts.reserve(work_.points.size());
        for (const RoutePoint& rp : work_.points)
            pts.push_back(vp.geoToScreen(rp.lat, rp.lon));

        p.setPen(QPen(kEditLine, 2.5));
        p.setBrush(Qt::NoBrush);
        for (size_t i = 1; i < pts.size(); ++i) p.drawLine(pts[i - 1], pts[i]);

        // Live leg distance + heading as the route is built or reshaped.
        for (size_t i = 1; i < pts.size(); ++i)
            drawLegLabel(p, work_.points[int(i) - 1], work_.points[int(i)],
                         pts[i - 1], pts[i], distUnit, brgMode, variation, cull);

        for (int i = 0; i < int(pts.size()); ++i) {
            const bool sel = (i == selected_);
            p.setPen(QPen(QColor(0x40, 0x30, 0x00), 1.5));
            p.setBrush(kEditNodeFill);
            p.drawEllipse(pts[i], kEditNode, kEditNode);
            if (sel) {
                p.setPen(QPen(kSelRing, 2.5));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(pts[i], kEditNode + 4.0, kEditNode + 4.0);
            }
            // Sequence number for orientation.
            p.setPen(QColor(0x40, 0x30, 0x00));
            p.drawText(pts[i] + QPointF(kEditNode + 2, -kEditNode), QString::number(i + 1));
        }
    }

    // In-progress waypoint (edit) -------------------------------------------
    if (mode_ == Mode::EditWaypoint) {
        const QPointF s = vp.geoToScreen(editWpt_.lat, editWpt_.lon);
        const double a = 7.0;
        QPainterPath dia;
        dia.moveTo(s + QPointF(0, -a));
        dia.lineTo(s + QPointF(a, 0));
        dia.lineTo(s + QPointF(0, a));
        dia.lineTo(s + QPointF(-a, 0));
        dia.closeSubpath();
        p.setPen(QPen(QColor(0x40, 0x30, 0x00), 1.5));
        p.setBrush(kEditNodeFill);
        p.drawPath(dia);
        p.setPen(QPen(kSelRing, 2.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(s, a + 4.0, a + 4.0);
        if (!editWpt_.name.isEmpty())
            drawLabel(p, s + QPointF(a + 6, 0), editWpt_.name);
    }

    // Active (next) waypoint highlight ---------------------------------------
    // While a route is being navigated, ring the destination the navigator is
    // steering to. Drawn last so it sits above the route nodes/legs.
    if (nav_) {
        const NavigationData& n = nav_->navigation();
        if (n.active) {
            const QPointF s = vp.geoToScreen(n.destinationLatDeg, n.destinationLonDeg);
            if (onScreen(s, cull)) {
                p.setPen(QPen(kSelRing, 2.5));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(s, kActiveRing, kActiveRing);
            }
        }
    }
}

// ---- picking / editing -----------------------------------------------------

int RouteOverlay::nodeAt(const QPointF& screenPt) const {
    if (!haveVp_) return -1;
    // EditWaypoint has a single draggable node (index 0): the waypoint itself.
    if (mode_ == Mode::EditWaypoint) {
        const QPointF s = vp_.geoToScreen(editWpt_.lat, editWpt_.lon);
        const double dx = s.x() - screenPt.x(), dy = s.y() - screenPt.y();
        return (dx * dx + dy * dy <= kPickRadiusPx * kPickRadiusPx) ? 0 : -1;
    }
    double bestSq = kPickRadiusPx * kPickRadiusPx;
    int best = -1;
    for (int i = 0; i < work_.points.size(); ++i) {
        const QPointF s = vp_.geoToScreen(work_.points[i].lat, work_.points[i].lon);
        const double dx = s.x() - screenPt.x(), dy = s.y() - screenPt.y();
        const double dSq = dx * dx + dy * dy;
        if (dSq < bestSq) { bestSq = dSq; best = i; }
    }
    return best;
}

// Square distance from a point to a line segment (in screen pixels). Used to
// hit-test a tap against a route leg.
namespace {
double pointToSegmentSq(const QPointF& p, const QPointF& a, const QPointF& b) {
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    const double len2 = dx * dx + dy * dy;
    if (len2 <= 1e-9) {
        const double ex = p.x() - a.x(), ey = p.y() - a.y();
        return ex * ex + ey * ey;
    }
    double t = ((p.x() - a.x()) * dx + (p.y() - a.y()) * dy) / len2;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    const double cx = a.x() + t * dx, cy = a.y() + t * dy;
    const double ex = p.x() - cx, ey = p.y() - cy;
    return ex * ex + ey * ey;
}
}  // namespace

bool RouteOverlay::hitTest(const QPointF& screenPt) {
    if (!haveVp_) return false;
    if (mode_ == Mode::CreateRoute || mode_ == Mode::EditRoute) {
        // A tap that reached hitTest is empty-area (node taps are grabbed by
        // onPress): append a new point at the end of the working route.
        double lat = 0.0, lon = 0.0;
        vp_.screenToGeo(screenPt, lat, lon);
        RoutePoint rp; rp.lat = lat; rp.lon = lon;
        work_.points.push_back(rp);
        selected_ = int(work_.points.size()) - 1;
        notifySelection();
        repaint();
        return true;
    }
    if (mode_ == Mode::CreateWaypoint) {
        double lat = 0.0, lon = 0.0;
        vp_.screenToGeo(screenPt, lat, lon);
        if (onWaypointPlaced_) onWaypointPlaced_(lat, lon);
        return true;
    }
    // Idle mode: hit-test saved waypoints and route nodes/legs so the user can
    // tap them to open a quick-info popup (mirrors the AIS overlay's click
    // pattern). Waypoints win on tie because they're more specific. Hidden
    // objects don't pick — out of sight, out of mind.
    if (!store_ || !onObjectClicked_) return false;
    constexpr double kPickPx     = 14.0;
    constexpr double kLegPickPx  = 10.0;
    const double pickSq    = kPickPx    * kPickPx;
    const double legPickSq = kLegPickPx * kLegPickPx;

    qint64 bestId   = -1;
    double bestSq   = pickSq;
    int    bestPointIdx = -1;           // route point index of the winning hit
    ClickedRouteObject::Kind bestKind = ClickedRouteObject::Kind::Waypoint;

    for (const Waypoint& w : store_->waypoints()) {
        if (!w.visible) continue;
        const QPointF s = vp_.geoToScreen(w.lat, w.lon);
        const double dx = s.x() - screenPt.x(), dy = s.y() - screenPt.y();
        const double dSq = dx * dx + dy * dy;
        if (dSq < bestSq) {
            bestSq = dSq; bestId = w.id;
            bestKind = ClickedRouteObject::Kind::Waypoint;
            bestPointIdx = -1;          // standalone waypoint, not a route point
        }
    }
    // Routes: nodes get the same tight radius, legs a slightly slacker one.
    // Run the leg pass second so a node hit (already within pickSq) wins over
    // an adjacent leg hit at the same tap. A node hit records its own index; a
    // leg hit records the leg's end (destination) node.
    for (const Route& r : store_->routes()) {
        if (!r.visible || r.points.isEmpty()) continue;
        for (int i = 0; i < r.points.size(); ++i) {
            const QPointF s = vp_.geoToScreen(r.points[i].lat, r.points[i].lon);
            const double dx = s.x() - screenPt.x(), dy = s.y() - screenPt.y();
            const double dSq = dx * dx + dy * dy;
            if (dSq < bestSq) {
                bestSq = dSq; bestId = r.id;
                bestKind = ClickedRouteObject::Kind::Route;
                bestPointIdx = i;
            }
        }
    }
    for (const Route& r : store_->routes()) {
        if (!r.visible || r.points.size() < 2) continue;
        // Only consider legs when nothing closer already won.
        QPointF prev = vp_.geoToScreen(r.points.first().lat, r.points.first().lon);
        for (int i = 1; i < r.points.size(); ++i) {
            const QPointF cur = vp_.geoToScreen(r.points[i].lat, r.points[i].lon);
            const double dSq = pointToSegmentSq(screenPt, prev, cur);
            if (dSq < legPickSq && dSq < bestSq) {
                bestSq = dSq; bestId = r.id;
                bestKind = ClickedRouteObject::Kind::Route;
                bestPointIdx = i;       // head toward the leg's end node
            }
            prev = cur;
        }
    }
    if (bestId < 0) return false;       // nothing under the tap
    ClickedRouteObject hit;
    hit.kind = bestKind;
    hit.id   = bestId;
    hit.screenPt = screenPt;
    hit.pointIndex = bestPointIdx;
    onObjectClicked_(hit);
    return true;
}

bool RouteOverlay::onPress(const QPointF& screenPt) {
    if (mode_ != Mode::CreateRoute && mode_ != Mode::EditRoute
        && mode_ != Mode::EditWaypoint) return false;
    const int idx = nodeAt(screenPt);
    if (idx < 0) return false;          // empty press: let it pan / become a tap
    dragging_  = idx;
    dragMoved_ = false;
    pressPos_  = screenPt;
    return true;
}

void RouteOverlay::onMove(const QPointF& screenPt) {
    if (dragging_ < 0 || !haveVp_) return;
    if ((screenPt - pressPos_).manhattanLength() > 3.0) dragMoved_ = true;
    double lat = 0.0, lon = 0.0;
    vp_.screenToGeo(screenPt, lat, lon);
    if (mode_ == Mode::EditWaypoint) {
        editWpt_.lat = lat;
        editWpt_.lon = lon;
    } else {
        work_.points[dragging_].lat = lat;
        work_.points[dragging_].lon = lon;
    }
}

void RouteOverlay::onRelease(const QPointF& screenPt) {
    if (dragging_ < 0) return;
    (void)screenPt;
    if (mode_ == Mode::EditWaypoint) {
        dragging_ = -1;       // the single waypoint stays where released
        repaint();
        return;
    }
    // Route node: a tap (no movement) selects it; a drag leaves it where released
    // and also selects it so it can be deleted.
    selected_ = dragging_;
    dragging_ = -1;
    notifySelection();
    repaint();
}

// ---- session control -------------------------------------------------------

void RouteOverlay::beginCreateRoute() {
    mode_ = Mode::CreateRoute;
    work_ = Route{};
    work_.visible = true;
    selected_ = -1; dragging_ = -1;
    notifySelection();
    repaint();
}

void RouteOverlay::beginEditRoute(const Route& r) {
    mode_ = Mode::EditRoute;
    work_ = r;                 // copy, keeps id
    selected_ = -1; dragging_ = -1;
    notifySelection();
    repaint();
}

void RouteOverlay::beginCreateWaypoint() {
    mode_ = Mode::CreateWaypoint;
    work_ = Route{};
    selected_ = -1; dragging_ = -1;
    notifySelection();
    repaint();
}

void RouteOverlay::beginEditWaypoint(const Waypoint& w) {
    mode_ = Mode::EditWaypoint;
    editWpt_ = w;              // copy, keeps id
    work_ = Route{};
    selected_ = -1; dragging_ = -1;
    notifySelection();
    repaint();
}

void RouteOverlay::endEditing() {
    mode_ = Mode::None;
    work_ = Route{};
    editWpt_ = Waypoint{};
    selected_ = -1; dragging_ = -1;
    notifySelection();
    repaint();
}

void RouteOverlay::deleteSelectedNode() {
    if (selected_ < 0 || selected_ >= work_.points.size()) return;
    work_.points.remove(selected_);
    selected_ = -1;
    notifySelection();
    repaint();
}
