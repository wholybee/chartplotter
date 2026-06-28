#include "chart_view.hpp"
#include "chart_catalog.hpp"
#include "chart_source.hpp"
#include "projection.hpp"
#include "geom_clip.hpp"
#include "vessel_symbol.hpp"
#include "sym_atlas.hpp"
#include "theme.hpp"
#include "mbtiles_service.hpp"

#include <QPainter>
#include <QPaintEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QTimer>
#include <QPushButton>
#include <QScreen>
#include <QPen>
#include <QBrush>
#include <QPolygonF>
#include <QThread>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

QColor fillColor(const Feature& f) {
    if (f.kind == FeatureKind::LandArea) return QColor(217, 199, 148);
    if (!f.hasDepth)                     return QColor(184, 212, 235);
    double d = f.depth;
    if (d <  0.0)  return QColor(158, 189, 140);
    if (d <  2.0)  return QColor(102, 168, 217);
    if (d <  5.0)  return QColor(143, 194, 227);
    if (d < 10.0)  return QColor(184, 217, 240);
    if (d < 20.0)  return QColor(217, 235, 250);
    return QColor(242, 247, 255);
}

// Builds a path in scene coordinates: projected X, and Y negated so north is up.
QPainterPath buildPathFromRings(const std::vector<std::vector<Pt>>& rings, bool closed) {
    QPainterPath path;
    if (closed) path.setFillRule(Qt::OddEvenFill);
    for (const auto& ring : rings) {
        if (ring.size() < 2) continue;
        path.moveTo(ring[0].x, -ring[0].y);
        for (std::size_t i = 1; i < ring.size(); ++i)
            path.lineTo(ring[i].x, -ring[i].y);
        if (closed) path.closeSubpath();
    }
    return path;
}

// Area-weighted centroid of a polygon ring (projected coords). Falls back to
// the vertex average for a degenerate (near-zero-area) ring. Used to place an
// area feature's centred symbol (e.g. an anchorage's anchor glyph).
Pt ringCentroid(const std::vector<Pt>& ring) {
    const std::size_t n = ring.size();
    if (n == 0) return {0.0, 0.0};
    double a = 0.0, cx = 0.0, cy = 0.0;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const double cross = ring[j].x * ring[i].y - ring[i].x * ring[j].y;
        a  += cross;
        cx += (ring[j].x + ring[i].x) * cross;
        cy += (ring[j].y + ring[i].y) * cross;
    }
    if (std::abs(a) < 1e-9) {
        double sx = 0.0, sy = 0.0;
        for (const Pt& p : ring) { sx += p.x; sy += p.y; }
        return { sx / static_cast<double>(n), sy / static_cast<double>(n) };
    }
    a *= 0.5;
    return { cx / (6.0 * a), cy / (6.0 * a) };
}

using geom::clipRingToRect;
using geom::clipPolylineToRect;
using geom::pointInRect;

// Rough in-memory footprint of a parsed cell, for the LRU byte budget.
std::size_t approxBytes(const std::vector<Feature>& feats) {
    std::size_t b = sizeof(Feature) * feats.capacity();
    for (const Feature& f : feats) {
        b += sizeof(std::vector<Pt>) * f.rings.capacity();
        for (const auto& ring : f.rings)
            b += sizeof(Pt) * ring.capacity();
    }
    return b;
}

// Vertex-merge tolerance (projected metres) for a usage band, ~half a pixel at
// the band's display scale. Band-based so geometry never needs re-simplifying as
// you zoom within a band; the finest bands keep full detail.
double simplifyToleranceM(int band) {
    switch (band) {
        case 1: return 1389.0;   // overview
        case 2: return 278.0;    // general
        case 3: return 46.0;     // coastal
        case 4: return 13.9;     // approach
        case 5: return 2.8;      // harbour
        default: return 0.0;     // berthing / unknown: keep full detail
    }
}

// A cell's data-coverage footprint as a scene-frame QPainterPath, shifted into
// the common view frame by its wrap offset and clipped to `bound` (the kept
// region, also scene-frame). Scene Y is north-up, so y = -projected y. Falls
// back to the bbox rectangle when the cell has no M_COVR rings. Used by the
// quilting pass to decide, region by region, which band is the finest present.
QPainterPath coveragePath(const CellRecord& c, double off, const QPainterPath& bound) {
    QPainterPath pp;
    pp.setFillRule(Qt::WindingFill);
    if (!c.coverage.empty()) {
        for (const std::vector<Pt>& ring : c.coverage) {
            if (ring.size() < 3) continue;
            QPolygonF poly;
            poly.reserve(static_cast<int>(ring.size()));
            for (const Pt& p : ring) poly << QPointF(p.x + off, -p.y);
            pp.addPolygon(poly);
            pp.closeSubpath();
        }
    } else {
        const BBox& b = c.bbox;
        pp.addRect(QRectF(b.minx + off, -b.maxy, b.maxx - b.minx, b.maxy - b.miny));
    }
    return pp.intersected(bound);
}

// Resolve where the GSHHG basemap lives. Search order: an explicit override
// (the basemap-folder setting), next to the executable, the per-user and shared
// data locations, and finally the in-tree dev folder. The first root that holds
// a recognizable layout wins. All tiers present are returned; which one is used
// is chosen by zoom at draw time (see ChartView::desiredTier).
struct BasemapSource { QString root; QStringList tiers; };

QStringList tiersIn(const QString& root) {
    QStringList out;
    static const char* all[] = {"c", "l", "i", "h", "f"};
    for (const char* t : all) {
        const QString f = root + "/GSHHS_shp/" + t + "/GSHHS_" + t + "_L1.shp";
        if (QFileInfo::exists(f)) out << QString::fromLatin1(t);
    }
    return out;
}

BasemapSource resolveBasemap(const QString& override) {
    QStringList roots;
    if (!override.isEmpty()) roots << override;
    roots << QCoreApplication::applicationDirPath() + "/gshhg-shp";
    for (const QString& d : QStandardPaths::standardLocations(QStandardPaths::AppDataLocation))
        roots << d + "/gshhg-shp";
    for (const QString& d : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        roots << d + "/gshhg-shp";
#ifdef CHARTPLOTTER_SOURCE_DIR
    roots << QString::fromLatin1(CHARTPLOTTER_SOURCE_DIR) + "/gshhg-shp";
#endif
    for (const QString& r : roots) {
        const QStringList ts = tiersIn(r);
        if (!ts.isEmpty()) return { r, ts };
    }
    return {};
}

// Clip + simplify one cell to `clipBox` (projected, real frame) with vertex-merge
// tolerance `tol`, building its vector primitives sorted by z. Pure and
// Qt-value-only: runs on a worker. Used for both ENC cells and the basemap.
//
// atlas: optional pointer to the loaded SymAtlas used to resolve symbol indices
// for Point features.  nullptr is allowed (basemap cells have no point
// features; ENC cells fall back to the dot glyph when atlas is not loaded).
BuiltCell buildCell(const QString& path, const std::vector<Feature>& feats,
                    int band, const BBox& clipBox, double tol,
                    const SymAtlas* atlas = nullptr) {
    BuiltCell bc;
    bc.path    = path;
    bc.band    = band;
    bc.clipBox = clipBox;

    const double zb = static_cast<double>(band) * 1000.0;

    std::vector<std::vector<Pt>> clipBuf;
    std::vector<std::vector<Pt>> simpBuf;

    auto reduce = [&](const std::vector<std::vector<Pt>>& src, std::size_t minPts)
            -> const std::vector<std::vector<Pt>>& {
        if (tol <= 0.0) return src;
        simpBuf.clear();
        for (const auto& ring : src) {
            std::vector<Pt> s = geom::simplify(ring, tol);
            if (s.size() >= minPts) simpBuf.push_back(std::move(s));
        }
        return simpBuf;
    };

    // Emit the TX()/TE() labels a SymHit produced at scene position `pos`. When
    // the instruction carried no text but the feature has an OBJNAM, fall back
    // to a single name label (the pre-S-52-text behaviour) so named objects keep
    // their label.
    auto pushHitTexts = [&](const SymHit& hit, QPointF pos, int scaleMin,
                            const std::string& fallbackName) {
        if (!hit.texts.empty()) {
            for (const SymText& t : hit.texts) {
                BuiltText bt;
                bt.pos = pos; bt.text = t.text; bt.scaleMin = scaleMin;
                bt.hjust = t.hjust; bt.vjust = t.vjust;
                bt.xoffs = t.xoffs; bt.yoffs = t.yoffs;
                bt.color = QColor(t.r, t.g, t.b);
                bt.pointSize = t.pointSize;
                bc.texts.push_back(std::move(bt));
            }
        } else if (!fallbackName.empty()) {
            BuiltText bt;
            bt.pos = pos; bt.text = QString::fromStdString(fallbackName);
            bt.scaleMin = scaleMin;
            bc.texts.push_back(std::move(bt));
        }
    };

    for (const Feature& f : feats) {
        const bool doClip = clipBox.valid() && !clipBox.contains(f.bbox);

        switch (f.kind) {
            case FeatureKind::DepthArea:
            case FeatureKind::LandArea: {
                const std::vector<std::vector<Pt>>* rings = &f.rings;
                if (doClip) {
                    clipBuf.clear();
                    for (const auto& ring : f.rings) {
                        std::vector<Pt> c = clipRingToRect(ring, clipBox);
                        if (c.size() >= 3) clipBuf.push_back(std::move(c));
                    }
                    if (clipBuf.empty()) break;
                    rings = &clipBuf;
                }
                const std::vector<std::vector<Pt>>& use = reduce(*rings, 3);
                if (use.empty()) break;

                BuiltPath bp;
                bp.path   = buildPathFromRings(use, true);
                bp.bounds = bp.path.boundingRect();
                bp.z      = zb + f.zorder;
                bp.filled = true;
                bp.brush  = fillColor(f);
                if (f.kind == FeatureKind::LandArea) {
                    bp.hasPen = true; bp.penColor = QColor(115, 97, 64); bp.penWidth = 1.0;
                }
                bc.paths.push_back(std::move(bp));
                break;
            }
            case FeatureKind::OtherArea:
            case FeatureKind::DepthContour:
            case FeatureKind::Coastline:
            case FeatureKind::OtherLine: {
                // Resolve the S-52 lookup once for OtherArea / OtherLine
                // features.  For OtherArea: any AC() fills the polygon, LS()
                // styles the boundary, and any SY() drops at the centroid.
                // For OtherLine: LS() styles the line itself.
                SymHit hit;
                const bool resolvable = atlas && !f.objClass.empty() &&
                    (f.kind == FeatureKind::OtherArea ||
                     f.kind == FeatureKind::OtherLine);
                if (resolvable) {
                    const SymGeom g = (f.kind == FeatureKind::OtherArea)
                        ? SymGeom::Area : SymGeom::Line;
                    hit = atlas->symbolForFeature(
                        QByteArray::fromStdString(f.objClass), g, f.attrs);
                }

                // When the area carries an AC() fill or an AP() pattern we need
                // closed polygons (Sutherland-Hodgman ring clip + closeSubpath);
                // otherwise the existing polyline clip is correct for outlines.
                const bool fillArea = (f.kind == FeatureKind::OtherArea) &&
                                      (hit.hasFill || hit.apIndex >= 0);

                const std::vector<std::vector<Pt>>* rings = &f.rings;
                if (doClip) {
                    clipBuf.clear();
                    if (fillArea) {
                        for (const auto& ring : f.rings) {
                            std::vector<Pt> c = clipRingToRect(ring, clipBox);
                            if (c.size() >= 3) clipBuf.push_back(std::move(c));
                        }
                    } else {
                        for (const auto& ring : f.rings) {
                            std::vector<std::vector<Pt>> runs = clipPolylineToRect(ring, clipBox);
                            for (auto& run : runs) clipBuf.push_back(std::move(run));
                        }
                    }
                    if (clipBuf.empty()) break;
                    rings = &clipBuf;
                }
                const std::vector<std::vector<Pt>>& use = reduce(*rings, fillArea ? 3 : 2);
                if (use.empty()) break;

                BuiltPath bp;
                bp.path    = buildPathFromRings(use, fillArea);
                bp.bounds  = bp.path.boundingRect();
                bp.z       = zb + f.zorder;
                bp.filled  = hit.hasFill;   // AC() wash (AP pattern overlays it)
                bp.apIndex = (f.kind == FeatureKind::OtherArea) ? hit.apIndex : -1;
                bp.lcIndex = hit.lcIndex;
                if (hit.hasFill)
                    bp.brush = QColor(hit.fill.r, hit.fill.g, hit.fill.b, hit.fill.a);
                bp.hasPen = true;
                if (hit.hasLine) {
                    bp.penColor = QColor(hit.line.r, hit.line.g, hit.line.b);
                    bp.penWidth = static_cast<qreal>(hit.line.width);
                    bp.penStyle = (hit.line.pattern == SymLineStyle::Dash) ? Qt::DashLine
                                : (hit.line.pattern == SymLineStyle::Dot)  ? Qt::DotLine
                                : Qt::SolidLine;
                }
                // A complex line (LC) replaces the plain outline; keep just a
                // faint guide so the boundary still reads if the motif is sparse.
                else if (hit.lcIndex >= 0)                        { bp.penColor = QColor(120, 120, 130, 90);  bp.penWidth = 0.6; }
                else if (hit.hasFill && bp.apIndex < 0)           { bp.hasPen = false; }   // fill-only area
                else if (f.kind == FeatureKind::Coastline)        { bp.penColor = QColor(64, 51, 31);         bp.penWidth = 1.4; }
                else if (f.kind == FeatureKind::DepthContour)     { bp.penColor = QColor(115, 153, 199);      bp.penWidth = 0.8; }
                else if (f.kind == FeatureKind::OtherArea)        { bp.penColor = QColor(102, 102, 115, 150); bp.penWidth = 0.7; }
                else                                              { bp.penColor = QColor(102, 102, 128);      bp.penWidth = 0.8; }
                bp.isDepthContour = (f.kind == FeatureKind::DepthContour);
                bc.paths.push_back(std::move(bp));

                // Area centred symbols (e.g. ACHARE anchor glyph, TSSLPT
                // direction arrow, restriction glyph from CS(RESTRN01)) and text
                // labels, placed at the polygon centroid so they stay put as the
                // viewport pans. Computed from the unclipped outer ring.
                if (f.kind == FeatureKind::OtherArea &&
                    !f.rings.empty() && !f.rings[0].empty()) {
                    const Pt c = ringCentroid(f.rings[0]);
                    const QPointF cp(c.x, -c.y);
                    for (const SymStamp& s : hit.symbols) {
                        if (s.symIdx == SymAtlas::kNoSymbol) continue;
                        BuiltSymbol bs;
                        bs.pos = cp; bs.symIdx = s.symIdx;
                        bs.rotationDeg = s.rotationDeg; bs.scaleMin = f.scaleMin;
                        bc.symbols.push_back(bs);
                    }
                    pushHitTexts(hit, cp, f.scaleMin, f.name);
                }
                break;
            }
            case FeatureKind::Sounding: {
                if (f.rings.empty() || f.rings[0].empty()) break;
                if (doClip && !pointInRect(f.rings[0][0], clipBox)) break;
                // Keep the raw depth (metres); the label is formatted at paint
                // time so the depth-unit preference is a repaint, not a rebuild.
                bc.soundings.push_back(
                    { QPointF(f.rings[0][0].x, -f.rings[0][0].y), f.depth, f.hasDepth,
                      f.scaleMin });
                break;
            }
            case FeatureKind::Point: {
                if (f.rings.empty() || f.rings[0].empty()) break;
                if (doClip && !pointInRect(f.rings[0][0], clipBox)) break;
                const QPointF pos(f.rings[0][0].x, -f.rings[0][0].y);
                SymHit hit;
                if (atlas)
                    hit = atlas->symbolForFeature(
                        QByteArray::fromStdString(f.objClass),
                        SymGeom::Point, f.attrs);
                // One BuiltSymbol per SY() stamp (lights add a flare, buoys add
                // a topmark, etc.). With no atlas / no resolved symbol, emit a
                // single dot-fallback marker.
                if (hit.symbols.empty()) {
                    BuiltSymbol bs; bs.pos = pos; bs.scaleMin = f.scaleMin;
                    bc.symbols.push_back(bs);
                } else {
                    for (const SymStamp& s : hit.symbols) {
                        BuiltSymbol bs;
                        bs.pos = pos; bs.symIdx = s.symIdx;
                        bs.rotationDeg = s.rotationDeg; bs.scaleMin = f.scaleMin;
                        bc.symbols.push_back(bs);
                    }
                }
                pushHitTexts(hit, pos, f.scaleMin, f.name);
                break;
            }
        }
    }

    std::sort(bc.paths.begin(), bc.paths.end(),
              [](const BuiltPath& a, const BuiltPath& b) { return a.z < b.z; });
    return bc;
}

} // namespace

ChartView::ChartView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);
    setAttribute(Qt::WA_OpaquePaintEvent, true);   // we fill the whole widget

    pool_.setMaxThreadCount(qBound(2, QThread::idealThreadCount(), 8));
    cache_.setLimits(256u * 1024u * 1024u, 256);
    cache_.setPinned([this](const QString& path) { return loaded_.contains(path); });

    // Basemap tiers: a small byte-budgeted LRU. The active tier is pinned so it
    // is never evicted; the cheap tiers (c/l/i) stay resident within budget while
    // the large ones (h/f) are dropped once you zoom away from them.
    tierCache_.setLimits(192u * 1024u * 1024u, 8);
    tierCache_.setPinned([this](const QString& tier) { return tier == basemapTier_; });

    // Long-press recognizer: started on press, cancelled if the user moves the
    // finger more than a few pixels or releases before the timeout. 500 ms is
    // the platform-conventional long-press threshold.
    longPressTimer_ = new QTimer(this);
    longPressTimer_->setSingleShot(true);
    longPressTimer_->setInterval(500);
    connect(longPressTimer_, &QTimer::timeout, this, [this] {
        // Suppress when an editor is active so route-edit gestures aren't
        // interrupted by a stray long-press.
        if (editor_ || editorGrab_) return;
        longPressFired_ = true;
        // The gesture is now a long-press, not a pan. Its handler typically opens
        // a popup menu that grabs the mouse, so this view will never receive the
        // matching release — end the drag here so a later move doesn't keep
        // panning the chart ("stuck in pan mode").
        dragging_ = false;
        setCursor(Qt::OpenHandCursor);
        emit longPressed(pressPos_);
    });

    updateTimer_ = new QTimer(this);
    updateTimer_->setSingleShot(true);
    updateTimer_->setInterval(120);
    connect(updateTimer_, &QTimer::timeout, this, [this] {
        // While a pan/zoom gesture is in flight, recomputing the visible cell set
        // and dispatching rebuilds is heavy enough to hitch the GUI thread — most
        // visibly as a stall when a slow drag lets this 120 ms debounce fire
        // between moves. Defer it until the gesture settles (aaTimer_); the view
        // already holds geometry 1.5× beyond the viewport to pan through.
        if (interacting_) return;
        updateVisibleCells();
        maybeBuildBasemap();
    });

    aaTimer_ = new QTimer(this);
    aaTimer_->setSingleShot(true);
    aaTimer_->setInterval(180);
    connect(aaTimer_, &QTimer::timeout, this, [this] {
        interacting_ = false;
        // Catch up on the cell management deferred during the gesture, then repaint
        // (which also restores antialiasing and the soundings/symbols overlay).
        updateVisibleCells();
        maybeBuildBasemap();
        update();
    });

    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(500);
    connect(saveTimer_, &QTimer::timeout, this, [this] {
        double lon, lat, scale;
        if (currentView(lon, lat, scale)) emit viewChanged(lon, lat, scale);
    });

    // Touch zoom buttons (lower-right, just left of the scale bar). Circular,
    // translucent so they sit nicely over the chart. Auto-repeat lets the user
    // hold to keep zooming on a phone/tablet. Colours follow the OS theme so the
    // glyph stays readable on both light and dark systems.
    const theme::OverlayBtnPalette& ob = theme::overlayBtn();
    auto makeZoomBtn = [this, &ob](const QString& glyph) {
        auto* b = new QPushButton(glyph, this);
        b->setFixedSize(48, 48);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);   // taps shouldn't steal keyboard focus
        b->setAutoRepeat(true);
        b->setAutoRepeatDelay(350);
        b->setAutoRepeatInterval(110);
        b->setStyleSheet(QStringLiteral(
            "QPushButton{ font-size:26px; font-weight:600; color:%1;"
            " border:1px solid %2; border-radius:24px; background:%3; }"
            "QPushButton:pressed{ background:%4; }")
            .arg(ob.fg, ob.border, ob.bg, ob.pressed));
        return b;
    };
    constexpr double kZoomStep = 1.15;
    zoomOutBtn_ = makeZoomBtn(QStringLiteral("−"));
    zoomInBtn_  = makeZoomBtn(QStringLiteral("+"));
    connect(zoomOutBtn_, &QPushButton::clicked, this, [this] { zoomBy(1.0 / kZoomStep); });
    connect(zoomInBtn_,  &QPushButton::clicked, this, [this] { zoomBy(kZoomStep); });
    zoomOutBtn_->show();
    zoomInBtn_->show();
    positionZoomButtons();

    // Load the symbol atlas from the standard data locations.
    // Search order mirrors the GDAL-data and basemap resolver patterns:
    //   1. next to the executable (installed / release builds)
    //   2. CHARTPLOTTER_SOURCE_DIR/data/ (in-tree development builds)
    auto tryLoadAtlas = [this](const QString& dir) {
        return symAtlas_.load(dir + QStringLiteral("/symbols.bin"),
                              dir + QStringLiteral("/rastersymbols-day.png"));
    };
    if (!tryLoadAtlas(QCoreApplication::applicationDirPath())) {
#ifdef CHARTPLOTTER_SOURCE_DIR
        tryLoadAtlas(QStringLiteral(CHARTPLOTTER_SOURCE_DIR) +
                     QStringLiteral("/data"));
#endif
    }

    // Tell the chart loader which S-57 attributes the symbology engine tests,
    // so it reads exactly those into each feature for best-match selection.
    // Done here (before any cell load) because the atlas is now resident and
    // its attribute set is immutable for the rest of the run.
    if (symAtlas_.isLoaded())
        chart::setSymbologyAttrs(symAtlas_.relevantAttrs());

    // MBTiles raster layer: a service object on its own thread does all SQLite
    // access and image decoding; we talk to it through queued signals/slots.
    mbThread_  = new QThread(this);
    mbService_ = new MbtilesService;          // no parent: moved to the worker
    mbService_->moveToThread(mbThread_);
    connect(this, &ChartView::rasterSetFolder,   mbService_, &MbtilesService::setFolder);
    connect(this, &ChartView::rasterRequestTile, mbService_, &MbtilesService::requestTile);
    connect(mbService_, &MbtilesService::discovered, this, &ChartView::onRasterDiscovered);
    connect(mbService_, &MbtilesService::tileReady,  this, &ChartView::onRasterTileReady);
    connect(mbService_, &MbtilesService::message,    this,
            [this](const QString& t) { emit statusChanged(t); });
    mbThread_->start();
}

ChartView::~ChartView() {
    // Stop the worker before tearing down: once the thread is joined no code runs
    // there, so deleting the service from this (GUI) thread is race-free.
    if (mbThread_) { mbThread_->quit(); mbThread_->wait(); }
    delete mbService_;
    mbService_ = nullptr;
}

void ChartView::setCatalog(ChartCatalog* catalog) {
    catalog_ = catalog;
    if (catalog_)
        connect(catalog_, &ChartCatalog::finished, this, &ChartView::onCatalogFinished);
}

// ---- camera ----------------------------------------------------------------

double ChartView::worldWidthM() { return 2.0 * proj::lonToX(180.0); }

// The most zoomed-out scale we allow: exactly enough that 360° of longitude
// fills the viewport width once. Zooming out past this would make the world
// narrower than the screen, so the wraparound would tile copies of the globe
// side by side. (The repeat is horizontal only, so this is keyed to width.)
double ChartView::minPpm() const {
    if (width() <= 0) return 1e-9;
    return width() / worldWidthM();
}

QTransform ChartView::cameraTransform() const {
    QTransform t;
    t.translate(width() / 2.0, height() / 2.0);
    t.scale(ppm_, ppm_);
    t.translate(-scx_, -scy_);
    return t;
}

QPointF ChartView::screenToScene(const QPointF& s) const {
    if (ppm_ <= 0.0) return QPointF();
    return QPointF((s.x() - width() / 2.0) / ppm_ + scx_,
                   (s.y() - height() / 2.0) / ppm_ + scy_);
}

void ChartView::normalizeCenter() {
    const double W = proj::lonToX(180.0);
    const double ww = worldWidthM();
    while (scx_ >=  W) scx_ -= ww;
    while (scx_ <  -W) scx_ += ww;
}

// The whole-world shift that brings a cell (centered at cellCenterX in scene X)
// nearest to the current view center — i.e. which side of the 180° seam to draw
// it on. Normally 0; ±worldWidth only for cells across the seam from the view.
double ChartView::wrapOffsetFor(double cellCenterX) const {
    const double ww = worldWidthM();
    return std::round((scx_ - cellCenterX) / ww) * ww;
}

QList<ChartObjectInfo> ChartView::pickObjects(const QPointF& screenPt) {
    QList<ChartObjectInfo> out;
    if (ppm_ <= 0.0) return out;

    const QPointF clickScene = screenToScene(screenPt);
    const double radius = 10.0 / ppm_;          // scene metres ≈ 10 device px
    const double radiusSq = radius * radius;

    // Distance² from (px,py) to segment (ax,ay)-(bx,by).
    auto segDistSq = [](double px, double py, double ax, double ay,
                        double bx, double by) -> double {
        const double dx = bx - ax, dy = by - ay;
        const double len2 = dx * dx + dy * dy;
        double t = (len2 > 0.0) ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        const double qx = ax + t * dx, qy = ay + t * dy;
        const double ex = px - qx, ey = py - qy;
        return ex * ex + ey * ey;
    };
    // Even-odd point-in-polygon test against one ring.
    auto pointInRing = [](double px, double py, const std::vector<Pt>& r) -> bool {
        bool in = false;
        for (std::size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) {
            const bool between = (r[i].y > py) != (r[j].y > py);
            if (between &&
                px < (r[j].x - r[i].x) * (py - r[i].y) / (r[j].y - r[i].y) + r[i].x)
                in = !in;
        }
        return in;
    };
    // Fallback object-class acronym for kinds the loader doesn't tag with one.
    auto classFor = [](FeatureKind k) -> const char* {
        switch (k) {
            case FeatureKind::Sounding:     return "SOUNDG";
            case FeatureKind::DepthContour: return "DEPCNT";
            case FeatureKind::Coastline:    return "COALNE";
            default:                        return "";
        }
    };

    struct Cand { ChartObjectInfo info; int prio; double distSq; };
    std::vector<Cand> cands;

    for (const QString& path : active_) {
        FeatureCache::FeaturesPtr feats = cache_.get(path);
        if (!feats) continue;
        const BBox bb = bboxByPath_.value(path);
        const double off = bb.valid() ? wrapOffsetFor((bb.minx + bb.maxx) / 2.0) : 0.0;
        const double cx = clickScene.x() - off;   // click in this cell's projected X
        const double cy = -clickScene.y();         // projected Y (north up)

        for (const Feature& f : *feats) {
            int prio = -1; double distSq = 0.0, repX = 0.0, repY = 0.0;
            switch (f.kind) {
                case FeatureKind::DepthArea:
                case FeatureKind::LandArea:
                    continue;                      // base canvas, not a query target
                case FeatureKind::Point:
                case FeatureKind::Sounding: {
                    if (f.rings.empty() || f.rings[0].empty()) continue;
                    const Pt& p = f.rings[0][0];
                    const double d = (p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy);
                    if (d > radiusSq) continue;
                    prio = 0; distSq = d; repX = p.x; repY = p.y;
                    break;
                }
                case FeatureKind::DepthContour:
                case FeatureKind::Coastline:
                case FeatureKind::OtherLine: {
                    double best = radiusSq; bool hit = false;
                    for (const auto& ring : f.rings)
                        for (std::size_t i = 1; i < ring.size(); ++i) {
                            const double d = segDistSq(cx, cy, ring[i - 1].x, ring[i - 1].y,
                                                       ring[i].x, ring[i].y);
                            if (d < best) { best = d; hit = true; repX = ring[i].x; repY = ring[i].y; }
                        }
                    if (!hit) continue;
                    prio = 1; distSq = best;
                    break;
                }
                case FeatureKind::OtherArea: {
                    if (f.rings.empty() || f.rings[0].size() < 3) continue;
                    if (!pointInRing(cx, cy, f.rings[0])) continue;
                    prio = 2; distSq = 0.0;
                    repX = f.rings[0][0].x; repY = f.rings[0][0].y;
                    break;
                }
            }
            if (prio < 0) continue;

            ChartObjectInfo info;
            info.objClass = f.objClass.empty() ? QString::fromLatin1(classFor(f.kind))
                                               : QString::fromStdString(f.objClass);
            info.name     = QString::fromStdString(f.name);
            info.kind     = f.kind;
            info.hasDepth = f.hasDepth;
            info.depthM   = f.depth;
            info.scaleMin = f.scaleMin;
            info.lon      = proj::xToLon(repX);
            info.lat      = proj::yToLat(repY);
            for (const auto& a : f.attrs)
                info.attrs.push_back({ QString::fromStdString(a.first),
                                       QString::fromStdString(a.second) });
            cands.push_back({ std::move(info), prio, distSq });
        }
    }

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.prio != b.prio) return a.prio < b.prio;
        return a.distSq < b.distSq;
    });
    constexpr int kMaxObjects = 10;
    for (const Cand& c : cands) {
        out.push_back(c.info);
        if (out.size() >= kMaxObjects) break;
    }
    return out;
}

void ChartView::restoreView(double lon, double lat, double s) {
    if (s <= 0.0) { fitToCatalog(); return; }
    scx_ = proj::lonToX(lon);
    scy_ = -proj::latToY(lat);
    ppm_ = std::max(s, minPpm());   // never restore past the whole-globe floor
    normalizeCenter();
    updatePointLOD();
    update();
}

bool ChartView::currentView(double& lon, double& lat, double& s) const {
    if (!haveContent() || ppm_ <= 0.0) return false;
    lon = proj::xToLon(scx_);
    lat = proj::yToLat(-scy_);
    s = ppm_;
    return true;
}

// ---- catalog / fit ---------------------------------------------------------

void ChartView::clearAll() {
    loaded_.clear();
    inFlight_.clear();
    building_.clear();
    wanted_.clear();
    active_.clear();
    drawClip_.clear();
    bandByPath_.clear();
    bboxByPath_.clear();
    cache_.clear();
    pointLodVisible_ = true;
}

void ChartView::onCatalogFinished(bool ok, const QString&) {
    ++generation_;
    clearAll();
    haveCatalog_ = ok && catalog_ && catalog_->bounds().valid();
    // NB: do NOT reset userInteracted_ here. It was already reset at scan start
    // (setRasterChartFolder), and the raster discovery runs in parallel: if it
    // finished first it may have already restored the saved view (setting
    // userInteracted_), and resetting now would let the auto-fit below clobber it.

    if (!haveCatalog_) {
        emit statusChanged(QString());
        update();
        return;
    }

    bandByPath_.reserve(static_cast<int>(catalog_->cells().size()));
    bboxByPath_.reserve(static_cast<int>(catalog_->cells().size()));
    for (const CellRecord& c : catalog_->cells()) {
        bandByPath_.insert(c.path, c.band);
        if (c.extentValid) bboxByPath_.insert(c.path, c.bbox);
    }

    if (havePendingView_) {
        restoreView(pendingLon_, pendingLat_, pendingScale_);
        havePendingView_ = false;
        userInteracted_ = true;
    } else if (!userInteracted_) {
        // No saved view — and none was applied by the parallel raster discovery,
        // which sets userInteracted_ when it restores one. Frame the whole set.
        fitToCatalog();
    }
    updateVisibleCells();
    maybeBuildBasemap();
    update();
}

void ChartView::fitToCatalog() {
    if (!haveCatalog_ || !catalog_ || width() <= 0 || height() <= 0) return;
    const BBox& b = catalog_->bounds();
    if (!b.valid()) return;
    const double wM = b.maxx - b.minx, hM = b.maxy - b.miny;
    if (wM <= 0.0 || hM <= 0.0) return;

    const double ppmW = (width()  * 0.92) / wM;
    const double ppmH = (height() * 0.92) / hM;
    ppm_ = std::max(1e-9, std::min(ppmW, ppmH));
    scx_ = (b.minx + b.maxx) / 2.0;
    scy_ = -(b.miny + b.maxy) / 2.0;
    normalizeCenter();
    updatePointLOD();
    update();
}

// Zoom/pan so the most detailed charts in the set fill the screen. Fitting the
// whole catalog (fitToCatalog) also frames the small-scale overview cell, which
// can cover a far larger area than the charts the user wants to see — so fit to
// the finest navigational band present, using each cell's real M_COVR coverage
// outline where available (tighter than its axis-aligned bbox). Falls back to
// the full catalog extent when no per-band extent is known.
void ChartView::zoomToCharts() {
    if (!haveCatalog_ || !catalog_ || width() <= 0 || height() <= 0) return;

    int finest = -1;
    for (const CellRecord& c : catalog_->cells())
        if (c.extentValid) finest = std::max(finest, c.band);

    BBox box;
    for (const CellRecord& c : catalog_->cells()) {
        if (!c.extentValid || c.band != finest) continue;
        if (!c.coverage.empty()) {
            for (const std::vector<Pt>& ring : c.coverage)
                for (const Pt& pt : ring) box.expand(pt.x, pt.y);
        } else {
            box.expand(c.bbox);
        }
    }
    if (!box.valid()) box = catalog_->bounds();   // fallback: whole set
    if (!box.valid()) return;

    const double wM = box.maxx - box.minx, hM = box.maxy - box.miny;
    if (wM <= 0.0 || hM <= 0.0) return;
    const double ppmW = (width()  * 0.92) / wM;
    const double ppmH = (height() * 0.92) / hM;
    ppm_ = std::max(1e-9, std::min(ppmW, ppmH));
    scx_ = (box.minx + box.maxx) / 2.0;
    scy_ = -(box.miny + box.maxy) / 2.0;   // catalog frame is north-up; scene negates Y
    userInteracted_ = true;                // explicit jump; don't auto-refit on resize
    normalizeCenter();
    updatePointLOD();
    update();
}

// Move the view center onto the ownship, leaving zoom untouched. Returns false
// (and does nothing) unless a fix is actually being displayed — the same
// conditions drawOwnship() uses. Shared by the one-shot menu action and the
// continuous auto-follow path.
bool ChartView::recenterOnOwnship() {
    if (ppm_ <= 0.0) return false;
    if (!ownship_.latitudeDeg.valid() || !ownship_.longitudeDeg.valid()) return false;

    scx_ = proj::lonToX(ownship_.longitudeDeg.value);
    scy_ = -proj::latToY(ownship_.latitudeDeg.value);
    normalizeCenter();
    scheduleUpdate();             // refresh cells/basemap (debounced)
    update();
    return true;
}

void ChartView::centerOnOwnship() {
    if (recenterOnOwnship()) {
        userInteracted_ = true;   // hold this center on resize instead of refitting
        saveTimer_->start();      // persist the new center (debounced)
    }
}

void ChartView::setAutoFollow(bool on) {
    if (on == autoFollow_) return;
    autoFollow_ = on;
    if (on) {
        userInteracted_ = true;
        recenterOnOwnship();      // jump to the boat now; stays armed if no fix yet
        saveTimer_->start();
    }
    emit autoFollowChanged(on);
}

// ---- viewport-driven cell selection ---------------------------------------

int ChartView::bandForVisibleWidth(double metres) {
    const double nm = metres / 1852.0;
    // Bands 1..6 keep their original ENC thresholds; 7..8 subdivide the most
    // zoomed-in range so CM93's finest scales (F, G) get distinct targets. ENC
    // (no bands >6) is unaffected: at these zooms its finest present band still
    // wins the quilt, exactly as when the tail returned 6.
    if (nm > 1500.0) return 1;
    if (nm >  300.0) return 2;
    if (nm >   50.0) return 3;
    if (nm >   15.0) return 4;
    if (nm >    3.0) return 5;
    if (nm >    1.0) return 6;
    if (nm >    0.3) return 7;
    return 8;
}

BBox ChartView::expandBox(const BBox& b, double frac) {
    double dx = (b.maxx - b.minx) * frac;
    double dy = (b.maxy - b.miny) * frac;
    BBox r;
    r.minx = b.minx - dx; r.maxx = b.maxx + dx;
    r.miny = b.miny - dy; r.maxy = b.maxy + dy;
    return r;
}

BBox ChartView::shiftX(const BBox& b, double dx) {
    BBox r = b;
    r.minx += dx; r.maxx += dx;
    return r;
}

void ChartView::scheduleUpdate() {
    if (ppm_ > 0.0) updateTimer_->start();   // cells need a catalog; basemap doesn't
}

bool ChartView::computeViewBoxes(BBox& view, BBox& wanted, BBox& keep, int& target) const {
    if (ppm_ <= 0.0 || width() <= 0) return false;
    const double halfW = (width()  / 2.0) / ppm_;
    const double halfH = (height() / 2.0) / ppm_;
    // Scene -> projected (north up): proj x = sx, proj y = -sy.
    view.minx = scx_ - halfW; view.maxx = scx_ + halfW;
    view.miny = -(scy_ + halfH); view.maxy = -(scy_ - halfH);

    const double visWidthM = width() / ppm_;
    // Detail-level bias: bands span roughly a factor of ~4 in visible width, so
    // a +1 step multiplies the effective width by 1/4 (one band more detail);
    // -1 multiplies by 4 (one band less). Half-steps interpolate smoothly.
    const double effWidthM = visWidthM * std::pow(4.0, -chartDetailLevel_);
    target = bandForVisibleWidth(effWidthM);
    wanted = expandBox(view, 0.5);
    keep   = expandBox(view, 1.5);
    return true;
}

void ChartView::updateVisibleCells() {
    if (!haveCatalog_ || !catalog_) return;

    BBox view, wantedArea, keepArea;
    int target;
    if (!computeViewBoxes(view, wantedArea, keepArea, target)) return;

    const std::vector<CellRecord>& cells = catalog_->cells();

    // Which bands have coverage in view? (wrap-aware via per-cell offset.)
    bool present[kMaxBand + 1] = {};
    for (const CellRecord& c : cells) {
        if (!c.extentValid || c.band < 1 || c.band > kMaxBand) continue;
        const double off = wrapOffsetFor((c.bbox.minx + c.bbox.maxx) / 2.0);
        if (shiftX(c.bbox, off).intersects(wantedArea)) present[c.band] = true;
    }

    int maxBand = target;
    bool haveAtOrBelow = false;
    for (int b = 1; b <= target; ++b) if (present[b]) { haveAtOrBelow = true; break; }
    if (!haveAtOrBelow)
        for (int b = target + 1; b <= kMaxBand; ++b) if (present[b]) { maxBand = b; break; }

    // --- Quilting: only the finest band draws in any given region ------------
    // Walk candidate cells finest-band-first, accumulating a "covered" region.
    // A cell contributes only the part of its coverage that a finer band has not
    // already claimed; fully-covered cells are dropped, and partially-covered
    // cells get a clip path so their geometry and symbols draw only where they
    // are the finest data. Coarser bands then fill only the gaps — eliminating
    // the stacked-duplicate symbols that came from drawing every band.
    //
    // Region math runs in a common scene frame (each cell shifted by its wrap
    // offset into the view frame); the resulting clip is stored back in the
    // cell's own un-shifted frame so it composes with drawOffsetX when painted.
    // The work is bounded to keepArea (the 1.5x kept region) so the clips stay
    // valid for the whole loaded set, not just the tighter wanted area.
    const QRectF keepScene(keepArea.minx, -keepArea.maxy,
                           keepArea.maxx - keepArea.minx,
                           keepArea.maxy - keepArea.miny);
    QPainterPath keepClip;
    keepClip.addRect(keepScene);

    struct Cand { const CellRecord* rec; double off; };
    std::vector<Cand> cands;
    for (const CellRecord& c : cells) {
        if (!c.extentValid || c.band < 1 || c.band > maxBand) continue;
        const double off = wrapOffsetFor((c.bbox.minx + c.bbox.maxx) / 2.0);
        if (shiftX(c.bbox, off).intersects(keepArea)) cands.push_back({&c, off});
    }
    // Finest first: a higher band number is finer (harbour over coastal etc.).
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.rec->band > b.rec->band; });

    active_.clear();
    drawClip_.clear();
    QPainterPath covered;                  // union of finer coverage, common frame
    covered.setFillRule(Qt::WindingFill);

    // Band-0 cells (filename didn't yield a usage band) can't be reasoned about,
    // so they always contribute, unclipped.
    for (const CellRecord& c : cells) {
        if (!c.extentValid || c.band != 0) continue;
        const double off = wrapOffsetFor((c.bbox.minx + c.bbox.maxx) / 2.0);
        if (shiftX(c.bbox, off).intersects(keepArea)) active_.insert(c.path);
    }

    for (std::size_t i = 0; i < cands.size();) {
        // Process a whole band against the coverage of strictly-finer bands, so
        // same-band cells (which tile without overlap) never clip each other.
        const int band = cands[i].rec->band;
        const QPainterPath coveredByFiner = covered;
        QPainterPath bandUnion;
        bandUnion.setFillRule(Qt::WindingFill);
        std::size_t j = i;
        for (; j < cands.size() && cands[j].rec->band == band; ++j) {
            const CellRecord& c = *cands[j].rec;
            const double off = cands[j].off;
            const QPainterPath cov = coveragePath(c, off, keepClip);
            if (cov.isEmpty()) continue;
            bandUnion.addPath(cov);

            if (coveredByFiner.isEmpty() || !coveredByFiner.intersects(cov)) {
                active_.insert(c.path);                 // open: contributes, no clip
                continue;
            }
            const QPainterPath contrib = cov.subtracted(coveredByFiner);
            if (contrib.isEmpty()) continue;            // fully hidden: drop
            active_.insert(c.path);
            drawClip_.insert(c.path, contrib.translated(-off, 0.0));   // cell frame
        }
        covered.addPath(bandUnion);
        i = j;
    }

    // Wanted set = contributing cells whose footprint reaches the tighter wanted
    // area (the 0.5x margin that triggers loading, as before).
    wanted_.clear();
    for (const CellRecord& c : cells) {
        if (!c.extentValid || !active_.contains(c.path)) continue;
        const double off = wrapOffsetFor((c.bbox.minx + c.bbox.maxx) / 2.0);
        if (shiftX(c.bbox, off).intersects(wantedArea)) wanted_.insert(c.path);
    }

    // Bring in newly-wanted cells. Each is built clipped in its own (real) frame;
    // the wrap offset is recorded so it can be drawn on the correct side of the
    // seam. keepArea shifted by -off puts the clip region into the cell's frame.
    for (const QString& path : wanted_) {
        if (loaded_.contains(path) || inFlight_.contains(path) ||
            building_.contains(path)) continue;
        const BBox bbox = bboxByPath_.value(path);
        const double off = wrapOffsetFor((bbox.minx + bbox.maxx) / 2.0);
        if (FeatureCache::FeaturesPtr feats = cache_.get(path))
            dispatchBuild(path, feats, bandByPath_.value(path, 0), shiftX(keepArea, -off), off);
        else
            dispatchLoad(path);
    }

    // Unload cells out of range; re-clip cells the view panned across (including
    // across the seam, where the offset — and thus the clip frame — changes).
    const QList<QString> loadedPaths = loaded_.keys();
    for (const QString& path : loadedPaths) {
        const int  band = bandByPath_.value(path, 0);
        const BBox bbox = bboxByPath_.value(path);
        const double off = wrapOffsetFor((bbox.minx + bbox.maxx) / 2.0);
        const bool bandOk = (band == 0) || (band >= 1 && band <= maxBand);
        // Drop cells out of range/band, and cells the quilt found fully covered
        // by a finer band (no longer in active_) so they stop drawing.
        const bool keep = bbox.valid() && bandOk && active_.contains(path) &&
                          shiftX(bbox, off).intersects(keepArea);
        if (!keep) { removeCell(path); continue; }

        const BBox clipBox = loaded_[path].clipBox;           // real frame
        const BBox wantedRealFrame = shiftX(wantedArea, -off);
        if (!clipBox.contains(wantedRealFrame) && !building_.contains(path)) {
            if (FeatureCache::FeaturesPtr feats = cache_.get(path))
                dispatchBuild(path, feats, band, shiftX(keepArea, -off), off);
        }
    }

    emit statusChanged(QStringLiteral("Bands ≤ %1  ·  %2 cell(s) shown")
                           .arg(maxBand).arg(loaded_.size()));
    update();
}

// ---- async load / build ----------------------------------------------------

void ChartView::dispatchLoad(const QString& path) {
    inFlight_.insert(path);
    const quint64 gen = generation_;
    const std::string p = path.toStdString();
    // Snapshot the active source at dispatch time; the worker holds this pointer,
    // so it must outlive the task (guaranteed: onChartSourceUnregistered drains
    // the pool before a source is destroyed).
    IChartSource* src = chartSource_;

    auto* watcher = new QFutureWatcher<CellLoadResult>(this);
    connect(watcher, &QFutureWatcher<CellLoadResult>::finished, this, [this, watcher, gen]() {
        CellLoadResult r = watcher->result();
        watcher->deleteLater();
        onCellLoaded(std::move(r), gen);
    });
    watcher->setFuture(QtConcurrent::run(&pool_, [p, path, src]() {
        CellLoadResult r;
        r.path = path;
        if (src) {
            // Plugin backend: the cell id is the QString path; geometry comes back
            // already projected, with S-57 object classes/attrs for symbology.
            QString err;
            r.ok = src->loadCell(path, r.features, r.bbox, err);
            r.error = err;
        } else {
            std::string err;
            r.ok = chart::loadCellFeatures(p, r.features, r.bbox, err);
            if (!r.ok) r.error = QString::fromStdString(err);
        }
        return r;
    }));
}

void ChartView::onChartSourceUnregistered(IChartSource* src) {
    if (chartSource_ != src) return;
    // Drain in-flight loadCell()/build workers before the source object dies:
    // bump the generation so any results that still post back are discarded,
    // drop queued tasks, then wait out the running ones (they hold `src`).
    ++generation_;
    pool_.clear();
    pool_.waitForDone();
    chartSource_ = nullptr;
    if (catalog_) catalog_->setSource(nullptr);
}

void ChartView::onCellLoaded(CellLoadResult r, quint64 gen) {
    if (gen != generation_) return;
    inFlight_.remove(r.path);
    if (!r.ok) return;

    const QString path = r.path;
    const int     band = bandByPath_.value(path, 0);

    auto feats = std::make_shared<std::vector<Feature>>(std::move(r.features));
    cache_.put(path, feats, approxBytes(*feats));

    if (!wanted_.contains(path)) return;
    if (loaded_.contains(path)) return;

    BBox view, wantedArea, keepArea;
    int target;
    if (!computeViewBoxes(view, wantedArea, keepArea, target)) return;
    const BBox bbox = bboxByPath_.value(path);
    const double off = wrapOffsetFor((bbox.minx + bbox.maxx) / 2.0);
    dispatchBuild(path, feats, band, shiftX(keepArea, -off), off);
}

void ChartView::dispatchBuild(const QString& path, FeatureCache::FeaturesPtr feats,
                              int band, const BBox& clipBox, double drawOffsetX) {
    if (!feats || building_.contains(path)) return;
    building_.insert(path);
    const quint64 gen = generation_;
    const QString p = path;

    // Pass a const pointer to the atlas: it is fully loaded before any worker
    // thread runs and is never modified afterwards, so no locking is needed.
    const SymAtlas* atlas = symAtlas_.isLoaded() ? &symAtlas_ : nullptr;

    auto* watcher = new QFutureWatcher<BuiltCell>(this);
    connect(watcher, &QFutureWatcher<BuiltCell>::finished, this,
            [this, watcher, gen, drawOffsetX]() {
        BuiltCell bc = watcher->result();
        watcher->deleteLater();
        onCellBuilt(std::move(bc), gen, drawOffsetX);
    });
    watcher->setFuture(QtConcurrent::run(&pool_, [p, feats, band, clipBox, atlas]() {
        return buildCell(p, *feats, band, clipBox, simplifyToleranceM(band), atlas);
    }));
}

void ChartView::onCellBuilt(BuiltCell bc, quint64 gen, double drawOffsetX) {
    building_.remove(bc.path);
    if (gen != generation_) return;
    if (!wanted_.contains(bc.path)) return;
    bc.drawOffsetX = drawOffsetX;
    storeCell(std::move(bc));
    emit statusChanged(QStringLiteral("%1 cell(s) shown").arg(loaded_.size()));
    update();
}

void ChartView::storeCell(BuiltCell bc) {
    loaded_.insert(bc.path, std::move(bc));   // replaces any existing
}

void ChartView::removeCell(const QString& path) {
    loaded_.remove(path);
    drawClip_.remove(path);
    active_.remove(path);
}

void ChartView::updatePointLOD() {
    if (ppm_ <= 0.0) return;
    const double visWidthM = width() / ppm_;
    // Apply the same bias as cell selection: positive detail levels raise the
    // threshold so symbols stay visible at wider zoom levels.
    const double effWidthM = visWidthM * std::pow(4.0, -chartDetailLevel_);
    const bool show = effWidthM < 20000.0;
    if (show == pointLodVisible_) return;
    pointLodVisible_ = show;
    update();
}

// ---- basemap (GSHHG land/lakes underlay) ----------------------------------

void ChartView::setBasemapDirectory(const QString& dir) {
    basemapDir_ = dir;
    reloadBasemap();
}

void ChartView::reloadBasemap() {
    const BasemapSource src = resolveBasemap(basemapDir_);
    basemapRoot_     = src.root;
    availableTiers_  = src.tiers;
    tierCache_.clear();
    basemapFeats_.reset();
    basemapTier_.clear();
    tierLoading_.clear();
    basemap_.clear();
    basemapClipBox_ = BBox();
    basemapBuiltPpm_ = 0.0;
    if (availableTiers_.isEmpty()) { update(); return; }
    ensureViewForBasemap();   // need a zoom to choose a tier
    ensureTierForZoom();      // load the tier appropriate for the current zoom
    update();
}

// With a basemap but no charts, establish a whole-world view so the underlay is
// visible on its own. A subsequent catalog load will fit to the charts instead.
void ChartView::ensureViewForBasemap() {
    if (ppm_ > 0.0 || availableTiers_.isEmpty() || width() <= 0 || height() <= 0) return;
    // The whole globe across the width, with no wraparound tiling — the same
    // floor the user can zoom out to. (On a landscape window this clips the
    // extreme polar latitudes, which is unavoidable without tiling the globe.)
    ppm_ = minPpm();
    scx_ = 0.0; scy_ = 0.0;
    updatePointLOD();
}

// The GSHHG tier whose nominal resolution best matches the current zoom (metres
// per pixel), clamped to the tiers actually installed: coarse when zoomed out,
// fine when zoomed in. Prefers the ideal tier, then a coarser one, then a finer.
QString ChartView::desiredTier() const {
    if (availableTiers_.isEmpty()) return QString();
    const double mpp = (ppm_ > 0.0) ? 1.0 / ppm_ : 1e9;   // metres per pixel
    QString ideal;
    if      (mpp <=   120.0) ideal = QStringLiteral("f");   // full   (~50 m)
    else if (mpp <=   600.0) ideal = QStringLiteral("h");   // high   (~200 m)
    else if (mpp <=  3000.0) ideal = QStringLiteral("i");   // interm.(~1 km)
    else if (mpp <= 15000.0) ideal = QStringLiteral("l");   // low    (~5 km)
    else                     ideal = QStringLiteral("c");   // crude  (~25 km)

    static const QStringList order = {"f", "h", "i", "l", "c"};   // fine -> coarse
    int idx = order.indexOf(ideal);
    if (idx < 0) idx = order.size() - 1;
    for (int j = idx; j < order.size(); ++j)        // ideal, then coarser
        if (availableTiers_.contains(order[j])) return order[j];
    for (int j = idx - 1; j >= 0; --j)              // else the nearest finer
        if (availableTiers_.contains(order[j])) return order[j];
    return availableTiers_.first();
}

void ChartView::ensureTierForZoom() {
    if (availableTiers_.isEmpty() || ppm_ <= 0.0) return;
    const QString want = desiredTier();
    if (want.isEmpty() || want == basemapTier_) return;

    if (FeatureCache::FeaturesPtr cached = tierCache_.get(want)) {
        basemapFeats_   = cached;     // switch instantly; old stays drawn until rebuilt
        basemapTier_    = want;
        basemapBuiltPpm_ = 0.0;       // force a rebuild at the new tier
        tierCache_.trim();            // active tier now pinned; shed others over budget
        return;
    }
    if (tierLoading_ == want) return; // already loading this one
    loadTier(want);
}

void ChartView::loadTier(const QString& tier) {
    if (basemapRoot_.isEmpty()) return;
    tierLoading_ = tier;
    const std::string root = basemapRoot_.toStdString();
    const std::string t = tier.toStdString();

    auto* watcher = new QFutureWatcher<FeatureCache::FeaturesPtr>(this);
    connect(watcher, &QFutureWatcher<FeatureCache::FeaturesPtr>::finished, this,
            [this, watcher, tier]() {
        FeatureCache::FeaturesPtr feats = watcher->result();
        watcher->deleteLater();
        onTierLoaded(feats, tier);
    });
    watcher->setFuture(QtConcurrent::run(&pool_, [root, t]() -> FeatureCache::FeaturesPtr {
        auto feats = std::make_shared<std::vector<Feature>>();
        std::string err;
        chart::loadBasemap(root, t, *feats, err);
        return feats;   // empty on failure
    }));
}

void ChartView::onTierLoaded(FeatureCache::FeaturesPtr feats, const QString& tier) {
    if (tierLoading_ == tier) tierLoading_.clear();
    if (!feats || feats->empty()) return;        // keep whatever we had
    tierCache_.put(tier, feats, approxBytes(*feats));
    if (desiredTier() == tier) {                 // still the right tier for this zoom
        basemapFeats_    = feats;
        basemapTier_     = tier;
        basemapBuiltPpm_ = 0.0;
        tierCache_.trim();                       // active pinned; shed others over budget
        maybeBuildBasemap();
    }
}

void ChartView::maybeBuildBasemap() {
    if (availableTiers_.isEmpty() || ppm_ <= 0.0) return;
    ensureTierForZoom();
    if (!basemapFeats_ || basemapBuilding_) return;

    BBox view, wantedArea, keepArea;
    int target;
    if (!computeViewBoxes(view, wantedArea, keepArea, target)) return;

    const bool zoomStale = basemapBuiltPpm_ <= 0.0 ||
        ppm_ > basemapBuiltPpm_ * 1.6 || ppm_ < basemapBuiltPpm_ * 0.6;
    if (!basemap_.empty() && !zoomStale && basemapClipBox_.contains(wantedArea))
        return;

    const double ww  = worldWidthM();
    const double W   = proj::lonToX(180.0);
    const double tol = 0.5 / ppm_;          // ~half a pixel, zoom-appropriate

    // Whole-world copies the view spans (normally {0}; ±1 near the date line).
    int kmin = static_cast<int>(std::ceil((keepArea.minx - W) / ww));
    int kmax = static_cast<int>(std::floor((keepArea.maxx + W) / ww));
    kmin = std::max(kmin, -2); kmax = std::min(kmax, 2);

    std::vector<std::pair<BBox, double>> reqs;   // (clip box real frame, offset)
    for (int k = kmin; k <= kmax; ++k)
        reqs.emplace_back(shiftX(keepArea, -k * ww), k * ww);
    if (reqs.empty()) return;

    basemapBuilding_ = true;
    basemapClipBox_  = keepArea;
    basemapBuiltPpm_ = ppm_;
    auto feats = basemapFeats_;

    auto* watcher = new QFutureWatcher<std::vector<BuiltCell>>(this);
    connect(watcher, &QFutureWatcher<std::vector<BuiltCell>>::finished, this,
            [this, watcher, feats]() {
        std::vector<BuiltCell> cells = watcher->result();
        watcher->deleteLater();
        onBasemapBuilt(std::move(cells), feats);
    });
    watcher->setFuture(QtConcurrent::run(&pool_, [feats, reqs, tol]() {
        std::vector<BuiltCell> result;
        result.reserve(reqs.size());
        for (const auto& r : reqs) {
            BuiltCell bc = buildCell(QString(), *feats, 0, r.first, tol);
            bc.drawOffsetX = r.second;
            result.push_back(std::move(bc));
        }
        return result;
    }));
}

void ChartView::onBasemapBuilt(std::vector<BuiltCell> cells, FeatureCache::FeaturesPtr feats) {
    basemapBuilding_ = false;
    if (feats != basemapFeats_) return;     // data was reloaded while building
    basemap_ = std::move(cells);
    update();
}

void ChartView::setShowSoundings(bool on) {
    if (on == showSoundings_) return;
    showSoundings_ = on; update();
}
void ChartView::setShowSymbols(bool on) {
    if (on == showSymbols_) return;
    showSymbols_ = on; update();
}
void ChartView::setShowText(bool on) {
    if (on == showText_) return;
    showText_ = on; update();
}
void ChartView::setShowDepthContours(bool on) {
    if (on == showDepthContours_) return;
    showDepthContours_ = on; update();
}

void ChartView::setHideSymbolsWhilePanning(bool on) {
    if (on == hideSymbolsWhilePanning_) return;
    hideSymbolsWhilePanning_ = on; update();
}

void ChartView::setShowRasterCharts(bool on) {
    if (on == showRasterCharts_) return;
    showRasterCharts_ = on; update();
}

void ChartView::setVectorOverlay(bool on) {
    if (on == vectorOverlay_) return;
    vectorOverlay_ = on; update();
}

// ---- raster (MBTiles) layer ------------------------------------------------

void ChartView::setRasterChartFolder(const QString& dir) {
    // New generation invalidates any in-flight discovery / tile replies.
    ++rasterGen_;
    rasterCharts_.clear();
    rasterSceneBounds_ = BBox{};
    tileCache_.clear();
    tileInFlight_.clear();
    tileAbsent_.clear();
    // A folder change is a fresh start: allow the next discovery (or the ENC
    // catalog) to frame the charts. Guards against the old folder's view
    // lingering over a new, geographically distant chart set. This is the single
    // per-scan reset of the flag — onCatalogFinished must not reset it again, or
    // it would clobber a saved view that the parallel raster path had restored.
    userInteracted_ = false;
    emit rasterSetFolder(dir, rasterGen_);
    update();
}

void ChartView::onRasterDiscovered(const QVector<MbtilesMeta>& charts, quint64 gen) {
    if (gen != rasterGen_) return;            // a newer folder superseded this
    rasterCharts_ = charts;
    rasterSceneBounds_ = BBox{};
    for (const MbtilesMeta& m : charts)
        if (m.sceneBounds.valid()) rasterSceneBounds_.expand(m.sceneBounds);

    // A pure-raster folder (no ENC cells) has no ENC-driven view — frame the
    // raster coverage so the charts are actually visible. A pending saved view
    // wins; once the user pans/zooms we leave their view alone. (When ENC cells
    // are present they drive the view instead.)
    if (!charts.isEmpty() && !haveCatalog_ && !userInteracted_) {
        if (havePendingView_) {
            restoreView(pendingLon_, pendingLat_, pendingScale_);
            havePendingView_ = false;
            userInteracted_ = true;
        } else if (rasterSceneBounds_.valid()) {
            fitToSceneBox(rasterSceneBounds_);
        }
    }
    emit rasterChartsChanged(charts.size());
    update();
}

void ChartView::onRasterTileReady(int chartId, int z, int x, int y,
                                  const QImage& img, quint64 gen) {
    if (gen != rasterGen_) return;
    const RasterTileKey k{chartId, z, x, y};
    tileInFlight_.remove(k);
    if (img.isNull()) {
        // Bound the negative cache so a long session over sparse coverage can't
        // grow it without limit.
        if (tileAbsent_.size() > 8192) tileAbsent_.clear();
        tileAbsent_.insert(k);
    } else {
        tileCache_.insert(k, QPixmap::fromImage(img));
    }
    update();
}

void ChartView::requestRasterTile(const RasterTileKey& k) {
    if (tileInFlight_.contains(k)) return;
    tileInFlight_.insert(k);
    emit rasterRequestTile(k.chart, k.z, k.x, k.y, rasterGen_);
}

// Frame a box given in the scene frame (x = lonToX, y = -latToY). Mirrors
// fitToCatalog, which takes a raw-projected box and negates Y itself.
void ChartView::fitToSceneBox(const BBox& b) {
    if (width() <= 0 || height() <= 0 || !b.valid()) return;
    const double wM = b.maxx - b.minx, hM = b.maxy - b.miny;
    if (wM <= 0.0 || hM <= 0.0) return;
    const double ppmW = (width()  * 0.92) / wM;
    const double ppmH = (height() * 0.92) / hM;
    ppm_ = std::max(1e-9, std::min(ppmW, ppmH));
    scx_ = (b.minx + b.maxx) / 2.0;
    scy_ = (b.miny + b.maxy) / 2.0;
    normalizeCenter();
    updatePointLOD();
    update();
}

// Pan/zoom to frame a geographic box (degrees). Builds the scene-frame box
// (x = lonToX, y = -latToY) with ~25% padding and defers to fitToSceneBox. A
// degenerate box (single point / zero span) is padded to a small minimum so a
// one-point route still lands on screen at a sensible zoom.
void ChartView::fitToGeoBox(double latMin, double lonMin, double latMax, double lonMax) {
    BBox b;
    b.expand(proj::lonToX(lonMin), -proj::latToY(latMin));
    b.expand(proj::lonToX(lonMax), -proj::latToY(latMax));
    if (!b.valid()) return;
    double wM = b.maxx - b.minx, hM = b.maxy - b.miny;
    // Pad so the route isn't jammed against the edges; floor the span so a
    // single-point box (or a perfectly N-S / E-W line) still has area to frame.
    const double padX = std::max(wM * 0.25, 500.0);
    const double padY = std::max(hM * 0.25, 500.0);
    b.minx -= padX; b.maxx += padX;
    b.miny -= padY; b.maxy += padY;
    userInteracted_ = true;        // an explicit jump; don't auto-refit on resize
    fitToSceneBox(b);
}

// Draw the raster charts in device space: choose each chart's native pyramid
// zoom from the current scale, blit cached tiles, request missing ones, and fall
// back to the nearest cached coarser ancestor so zoom/pan never flashes blank.
void ChartView::drawRasterCharts(QPainter& p, const QTransform& cam, const QRectF& vis) {
    if (!showRasterCharts_ || rasterCharts_.isEmpty() || ppm_ <= 0.0) return;

    constexpr int    kTilePx       = 256;   // logical tile edge, pixels
    constexpr int    kMaxCacheTiles = 384;  // ~working set; older tiles evicted
    const double ww = worldWidthM();
    const double W  = ww * 0.5;

    p.resetTransform();                       // draw with explicit device rects
    p.setRenderHint(QPainter::SmoothPixmapTransform, !interacting_);
    tileNeeded_.clear();

    for (int chartId = 0; chartId < rasterCharts_.size(); ++chartId) {
        const MbtilesMeta& m = rasterCharts_[chartId];

        // Native zoom: one tile (kTilePx) ≈ one tile-span of scene metres at the
        // current scale, i.e. 2^z ≈ worldWidth * ppm / kTilePx. Clamp to range.
        int z = static_cast<int>(std::lround(std::log2(ww * ppm_ / kTilePx)));
        z = std::clamp(z, m.minZoom, m.maxZoom);
        const int    n    = 1 << z;
        const double span = ww / n;           // scene metres per tile

        // Visible tile span. Columns wrap (longitude); rows clamp (no N/S wrap).
        const int col0 = static_cast<int>(std::floor((vis.left()   + W) / span));
        const int col1 = static_cast<int>(std::floor((vis.right()  + W) / span));
        int row0 = static_cast<int>(std::floor((vis.top()    + W) / span));
        int row1 = static_cast<int>(std::floor((vis.bottom() + W) / span));
        row0 = std::clamp(row0, 0, n - 1);
        row1 = std::clamp(row1, 0, n - 1);

        const QRectF coverRect = m.sceneBounds.valid()
            ? QRectF(m.sceneBounds.minx, m.sceneBounds.miny,
                     m.sceneBounds.maxx - m.sceneBounds.minx,
                     m.sceneBounds.maxy - m.sceneBounds.miny)
            : QRectF();

        for (int col = col0; col <= col1; ++col) {
            const int    tx = ((col % n) + n) % n;     // wrapped XYZ column
            const double sx = -W + col * span;         // scene X of this copy
            for (int row = row0; row <= row1; ++row) {
                const int    ty = row;
                const double sy = -W + row * span;

                // Cull against the chart's coverage using the tile's canonical
                // (un-wrapped) position; draw at the wrapped sx.
                if (!coverRect.isNull()) {
                    const QRectF canon(-W + tx * span, sy, span, span);
                    if (!canon.intersects(coverRect)) continue;
                }

                const RasterTileKey k{chartId, z, tx, ty};
                tileNeeded_.insert(k);
                const QRectF dev = cam.mapRect(QRectF(sx, sy, span, span));

                auto it = tileCache_.constFind(k);
                if (it != tileCache_.constEnd()) {
                    p.drawPixmap(dev, it.value(), it.value().rect());
                    continue;
                }
                if (!tileAbsent_.contains(k)) requestRasterTile(k);

                // Fallback: nearest cached coarser ancestor, sub-rectangled to
                // this tile's footprint, so the area isn't blank while loading.
                for (int L = 1; z - L >= m.minZoom; ++L) {
                    const int az = z - L;
                    const int ax = tx >> L, ay = ty >> L;
                    auto ait = tileCache_.constFind(RasterTileKey{chartId, az, ax, ay});
                    if (ait == tileCache_.constEnd()) continue;
                    const QPixmap& pm = ait.value();
                    const double cell = double(pm.width()) / (1 << L);
                    const QRectF src((tx - (ax << L)) * cell,
                                     (ty - (ay << L)) * cell, cell, cell);
                    p.drawPixmap(dev, pm, src);
                    break;
                }
            }
        }
    }

    // Evict tiles outside this frame's working set when over budget.
    if (tileCache_.size() > kMaxCacheTiles) {
        for (auto it = tileCache_.begin();
             it != tileCache_.end() && tileCache_.size() > kMaxCacheTiles; ) {
            if (tileNeeded_.contains(it.key())) ++it;
            else it = tileCache_.erase(it);
        }
    }
}

void ChartView::setChartDetailLevel(double level) {
    if (level < -2.0) level = -2.0;
    if (level >  2.0) level =  2.0;
    if (level == chartDetailLevel_) return;
    chartDetailLevel_ = level;
    updatePointLOD();   // symbol visibility threshold depends on detail level
    scheduleUpdate();   // cell band selection also depends on detail level
    update();           // re-thin soundings now, before the debounced rebuild
}

void ChartView::setChartScaminLevel(double level) {
    if (level < -1.0) level = -1.0;
    if (level >  1.0) level =  1.0;
    if (level == scaminLevel_) return;
    scaminLevel_ = level;
    // SCAMIN is filtered entirely at paint time, so a bias change is a repaint —
    // no cell rebuild or band/LOD recompute needed.
    update();
}

double ChartView::displayScaleDenominator() const {
    if (ppm_ <= 0.0) return 0.0;
    // Ground metres per pixel at the view-centre latitude: scene metres are true
    // ground metres at the equator and Mercator scales them by 1/cos(lat).
    const double latC = proj::yToLat(-scy_);
    const double cosLat = std::cos(latC * proj::kDeg2Rad);
    if (cosLat <= 1e-6) return 0.0;
    const double groundMPerPx = cosLat / ppm_;

    // Physical size of one (logical) screen pixel, in metres. physicalDotsPerInch
    // can be unreliable, so clamp to a sane window and fall back to 96 DPI; the
    // user's slider compensates for any residual error in the absolute scale.
    double dpi = 96.0;
    if (const QScreen* s = screen()) {
        const double d = s->physicalDotsPerInch();
        if (d > 30.0 && d < 1000.0) dpi = d;
    }
    const double dpr = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    const double screenMPerPx = (0.0254 / dpi) * dpr;   // metres per logical px
    if (screenMPerPx <= 0.0) return 0.0;
    return groundMPerPx / screenMPerPx;
}

// Sentinels for the slider extremes: hide every point object / show every point
// object regardless of SCAMIN. Finite values are real denominators to compare.
namespace { constexpr double kScaminHideAll = -1.0; constexpr double kScaminShowAll = -2.0; }

double ChartView::scaminEffectiveDenominator() const {
    // Hard endpoints: -1 hides all point objects, +1 shows them all.
    if (scaminLevel_ <= -0.999) return kScaminHideAll;
    if (scaminLevel_ >=  0.999) return kScaminShowAll;
    const double denom = displayScaleDenominator();
    if (denom <= 0.0) return kScaminShowAll;   // no usable zoom: don't hide
    // Bias the reference denominator by up to ±4 octaves across the interior of
    // the slider. Positive bias lowers the threshold (reveals objects with a
    // smaller SCAMIN, i.e. more detail); negative raises it (declutters).
    constexpr double kMaxOctaves = 4.0;
    return denom * std::pow(2.0, -kMaxOctaves * scaminLevel_);
}

bool ChartView::scaminPasses(int scaleMin, double effectiveDenom) const {
    if (effectiveDenom == kScaminShowAll) return true;    // +1: everything
    if (effectiveDenom == kScaminHideAll) return false;   // -1: nothing
    if (scaleMin <= 0) return true;                       // no SCAMIN: always show
    // S-57 rule: draw while the display scale is no smaller than SCAMIN, i.e.
    // the display denominator does not exceed SCAMIN.
    return static_cast<double>(scaleMin) >= effectiveDenom;
}

void ChartView::setSymbolScale(double scale) {
    if (scale < 0.5) scale = 0.5;
    if (scale > 3.0) scale = 3.0;
    if (scale == symbolScale_) return;
    symbolScale_ = scale;
    update();
}

void ChartView::setVesselScale(double scale) {
    if (scale < 0.5) scale = 0.5;
    if (scale > 3.0) scale = 3.0;
    if (scale == vesselScale_) return;
    vesselScale_ = scale;
    update();
}

void ChartView::setDepthUnit(DepthUnit u) {
    if (u == depthUnit_) return;
    depthUnit_ = u; update();   // soundings are relabelled on repaint
}

void ChartView::setDistanceUnit(DistanceUnit u) {
    if (u == distanceUnit_) return;
    distanceUnit_ = u; update();   // scale bar relabels on repaint
}

// Soundings come from S-57 in metres. Show one decimal in the shallows (where
// the extra precision matters) and whole units in deeper water.
QString ChartView::formatSounding(double depthM) const {
    const double v = (depthUnit_ == DepthUnit::Meters)
                       ? depthM
                       : depthM * units::kMetersToFeet;
    return QString::number(v, 'f', v < 10.0 ? 1 : 0);
}

double ChartView::soundingMinSpacing(double lineHeightPx) const {
    // No thinning at or below nominal detail: level 0 must look unchanged, and
    // negative levels already show sparser (lower-band) soundings.
    if (chartDetailLevel_ <= 0.0) return 0.0;
    // Spacing in label-height units, scaled by detail. Density falls as the
    // inverse square of spacing, so these constants roughly target the intent
    // that +1 keeps ~25% and +2 keeps ~10% of the un-thinned soundings
    // (assuming level-0 soundings sit ~one line-height apart on screen).
    // Tunable: raise the slope to thin more aggressively.
    return lineHeightPx * (0.85 + 1.15 * chartDetailLevel_);
}

void ChartView::setInitialView(double lon, double lat, double scale) {
    pendingLon_ = lon; pendingLat_ = lat; pendingScale_ = scale;
    havePendingView_ = (scale > 0.0);
}

void ChartView::keepCurrentViewOnNextLoad() {
    double lon, lat, scale;
    if (currentView(lon, lat, scale)) setInitialView(lon, lat, scale);
}

void ChartView::persistViewNow() {
    double lon, lat, scale;
    if (currentView(lon, lat, scale)) emit viewChanged(lon, lat, scale);
}

void ChartView::beginInteraction() {
    interacting_ = true;
    aaTimer_->start();
    saveTimer_->start();
}

// ---- painting --------------------------------------------------------------

void ChartView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(204, 224, 242));

    if (ppm_ <= 0.0) {     // no view established yet (no charts, no basemap)
        p.setPen(QColor(80, 80, 80));
        QFont f = p.font(); f.setPointSize(13); p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("Tap the menu button (top-left) to open a chart folder."));
        return;
    }

    p.setRenderHint(QPainter::Antialiasing, !interacting_);

    const QTransform cam = cameraTransform();
    const QRectF vis = QRectF(scx_ - (width()  / 2.0) / ppm_,
                              scy_ - (height() / 2.0) / ppm_,
                              width() / ppm_, height() / ppm_);

    QPen pen; pen.setCosmetic(true);
    // Draw one cell's vector geometry, shifted by its wrap offset.
    auto drawPaths = [&](const BuiltCell& c) {
        const double off = c.drawOffsetX;
        QTransform t = cam;
        if (off != 0.0) t.translate(off, 0.0);
        p.setTransform(t);

        // Quilt clip: where a finer band overlaps this cell, restrict it to the
        // region it still owns. The path is in the cell's own scene frame, so it
        // matches the transform just set.
        const auto clipIt = drawClip_.constFind(c.path);
        const bool clipped = (clipIt != drawClip_.constEnd());
        if (clipped) { p.save(); p.setClipPath(*clipIt); }

        const QRectF visFrame = vis.translated(-off, 0.0);   // cull in cell frame
        for (const BuiltPath& bp : c.paths) {
            if (bp.isDepthContour && !showDepthContours_) continue;
            if (!bp.bounds.intersects(visFrame)) continue;
            // Vector-overlay mode drops every area fill (land/water/other) so the
            // raster imagery beneath shows through; pens are kept, so coastlines
            // and other outlines still draw over the imagery.
            const bool fill = bp.filled && !vectorOverlay_;
            p.setBrush(fill ? QBrush(bp.brush) : QBrush(Qt::NoBrush));
            if (bp.hasPen) {
                pen.setColor(bp.penColor);
                pen.setWidthF(bp.penWidth);
                pen.setStyle(bp.penStyle);
                p.setPen(pen);
            }
            else           { p.setPen(Qt::NoPen); }
            p.drawPath(bp.path);
        }
        if (clipped) p.restore();
    };

    // 0) Basemap underlay (land/lakes) beneath everything; charts cover it where
    //    they exist. Skipped in vector-overlay mode, where its opaque land fill
    //    would hide the raster imagery that serves as the base instead.
    if (!vectorOverlay_)
        for (const BuiltCell& bc : basemap_) drawPaths(bc);

    // 0.5) Raster (MBTiles) charts above the basemap, below the ENC vector cells
    //      so vector detail and overlays stay on top. Drawn in device space, so
    //      it resets the transform; restore the camera for the cell loop below.
    drawRasterCharts(p, cam, vis);
    p.setTransform(cam);

    // 1) Chart cells, coarser bands first so finer detail draws on top.
    std::vector<const BuiltCell*> order;
    order.reserve(loaded_.size());
    for (auto it = loaded_.constBegin(); it != loaded_.constEnd(); ++it)
        order.push_back(&it.value());
    std::sort(order.begin(), order.end(),
              [](const BuiltCell* a, const BuiltCell* b) { return a->band < b->band; });
    for (const BuiltCell* c : order) drawPaths(*c);

    // Quilt clips mapped into device space: geometry from a partially-covered
    // cell is suppressed where a finer band overlays it (the finer cell draws
    // its own there). Computed once, reused by the pattern/line-complex pass and
    // the sounding/symbol/text passes below.
    QHash<QString, QPainterPath> deviceClip;
    for (const BuiltCell* c : order) {
        const auto it = drawClip_.constFind(c->path);
        if (it == drawClip_.constEnd()) continue;
        QTransform t = cam;
        if (c->drawOffsetX != 0.0) t.translate(c->drawOffsetX, 0.0);
        deviceClip.insert(c->path, t.map(*it));
    }

    // 1.5) S-52 complex symbology at constant on-screen size, in device space:
    //      AP() area patterns (a motif tiled inside the area) and LC() complex
    //      lines (a motif stamped along the path). Both render through the atlas
    //      from the baked HPGL/raster definitions. Skipped mid-gesture, like the
    //      point overlays, so a pan/zoom frame stays cheap.
    if (symAtlas_.isLoaded() && !interacting_) {
        p.resetTransform();   // device-space clips/anchors below
        const QRectF visDev = rect().adjusted(-48, -48, 48, 48);
        for (const BuiltCell* c : order) {
            const double off = c->drawOffsetX;
            QTransform t = cam;
            if (off != 0.0) t.translate(off, 0.0);
            const QRectF visFrame = vis.translated(-off, 0.0);   // cull in cell frame
            const auto dcIt = deviceClip.constFind(c->path);
            const bool clipped = (dcIt != deviceClip.constEnd());
            const QPointF anchor = t.map(QPointF(0.0, 0.0));      // stable tiling origin

            for (const BuiltPath& bp : c->paths) {
                if (bp.apIndex < 0 && bp.lcIndex < 0) continue;
                if (!bp.bounds.intersects(visFrame)) continue;
                if (clipped) { p.save(); p.setClipPath(*dcIt); }
                if (bp.apIndex >= 0) {
                    const QPainterPath dev = t.map(bp.path);
                    if (dev.boundingRect().intersects(visDev))
                        symAtlas_.fillAreaPattern(p, bp.apIndex, dev, anchor,
                                                  static_cast<float>(symbolScale_));
                }
                if (bp.lcIndex >= 0) {
                    // Break the path into device-space polylines (one per
                    // subpath) and stamp the line-complex along each.
                    QPolygonF poly;
                    auto flush = [&]() {
                        if (poly.size() >= 2)
                            symAtlas_.drawLineComplex(p, bp.lcIndex, poly,
                                                      static_cast<float>(symbolScale_));
                        poly.clear();
                    };
                    const int n = bp.path.elementCount();
                    for (int i = 0; i < n; ++i) {
                        const QPainterPath::Element e = bp.path.elementAt(i);
                        if (e.isMoveTo()) flush();
                        poly << t.map(QPointF(e.x, e.y));
                    }
                    flush();
                }
                if (clipped) p.restore();
            }
        }
    }

    // 2) Soundings / symbols / text at constant on-screen size, in device space.
    //
    // These are the dominant per-frame cost at high detail: every sounding is a
    // drawText (CPU glyph rasterization) and every symbol a pixmap blit, and a
    // positive detail level pulls in thousands of them. Skip them while a pan or
    // zoom gesture is in flight so the moving frame draws only vector geometry;
    // they snap back when the gesture settles (aaTimer_ clears interacting_ and
    // repaints, the same mechanism that restores antialiasing).
    p.resetTransform();
    // Point overlays show when the zoom allows them (pointLodVisible_), except
    // during a gesture if the user opted to hide them for a faster moving frame.
    if (pointLodVisible_ && (!interacting_ || !hideSymbolsWhilePanning_)) {
        const QRectF screen = rect().adjusted(-24, -24, 24, 24);

        // SCAMIN declutter threshold for this frame: point objects (soundings
        // and symbols) whose SCAMIN is smaller than this are dropped. Computed
        // once here from the current zoom and the user's bias slider.
        const double scaminDenom = scaminEffectiveDenominator();
        if (showSoundings_) {
            QFont f = p.font(); f.setPointSizeF(8.0); p.setFont(f);
            // White soundings in vector-overlay mode read better over dark
            // satellite imagery than the usual deep-blue ink.
            p.setPen(vectorOverlay_ ? QColor(255, 255, 255) : QColor(26, 51, 115));
            const QFontMetricsF fm(f);
            const double asc = fm.ascent();

            // Detail-driven decluttering: keep a greedy minimum gap between drawn
            // soundings so the denser ones pulled in at higher detail don't pile
            // on top of each other. Works in screen pixels, so it's zoom-aware —
            // soundings reappear as you zoom in and they spread apart. A spatial
            // hash (cell = gap) keeps the "is anything already near here?" test
            // O(1) per sounding. minGap == 0 keeps every sounding (level <= 0).
            const double minGap = soundingMinSpacing(fm.height());
            const double minSq  = minGap * minGap;
            const double cell   = (minGap > 0.0) ? minGap : 1.0;
            std::unordered_map<qint64, std::vector<QPointF>> kept;
            auto cellKey = [cell](const QPointF& d) -> std::pair<int,int> {
                return { static_cast<int>(std::floor(d.x() / cell)),
                         static_cast<int>(std::floor(d.y() / cell)) };
            };
            auto farEnough = [&](const QPointF& d) -> bool {
                if (minGap <= 0.0) return true;
                const auto [gx, gy] = cellKey(d);
                for (int ix = gx - 1; ix <= gx + 1; ++ix)
                    for (int iy = gy - 1; iy <= gy + 1; ++iy) {
                        const qint64 key = (static_cast<qint64>(ix) << 32)
                                         ^ static_cast<quint32>(iy);
                        const auto it = kept.find(key);
                        if (it == kept.end()) continue;
                        for (const QPointF& q : it->second) {
                            const double dx = q.x() - d.x(), dy = q.y() - d.y();
                            if (dx * dx + dy * dy < minSq) return false;
                        }
                    }
                return true;
            };
            auto remember = [&](const QPointF& d) {
                const auto [gx, gy] = cellKey(d);
                const qint64 key = (static_cast<qint64>(gx) << 32)
                                 ^ static_cast<quint32>(gy);
                kept[key].push_back(d);
            };

            for (const BuiltCell* c : order) {
                const auto dcIt = deviceClip.constFind(c->path);
                const bool clipped = (dcIt != deviceClip.constEnd());
                if (clipped) { p.save(); p.setClipPath(*dcIt); }
                const double off = c->drawOffsetX;
                for (const Sounding& s : c->soundings) {
                    if (!scaminPasses(s.scaleMin, scaminDenom)) continue;
                    const QPointF d = cam.map(QPointF(s.pos.x() + off, s.pos.y()));
                    if (!screen.contains(d)) continue;
                    if (!farEnough(d)) continue;
                    const QString text = s.hasDepth ? formatSounding(s.depthM)
                                                    : QStringLiteral(".");
                    p.drawText(QPointF(d.x() + 1.0, d.y() + asc), text);
                    remember(d);
                }
                if (clipped) p.restore();
            }
        }
        if (showSymbols_) {
            // Pre-configure the fallback dot style; atlas blits don't use
            // pen/brush so setting them here doesn't interfere.
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(179, 26, 128));

            const bool atlasOk = symAtlas_.isLoaded();
            for (const BuiltCell* c : order) {
                const auto dcIt = deviceClip.constFind(c->path);
                const bool clipped = (dcIt != deviceClip.constEnd());
                if (clipped) { p.save(); p.setClipPath(*dcIt); }
                const double off = c->drawOffsetX;
                for (const BuiltSymbol& sym : c->symbols) {
                    if (!scaminPasses(sym.scaleMin, scaminDenom)) continue;
                    const QPointF d = cam.map(
                        QPointF(sym.pos.x() + off, sym.pos.y()));
                    if (!screen.contains(d)) continue;

                    if (atlasOk && sym.symIdx != SymAtlas::kNoSymbol) {
                        symAtlas_.draw(p, sym.symIdx, d, sym.rotationDeg,
                                       static_cast<float>(symbolScale_));
                    } else {
                        // Fallback: magenta dot (pen/brush set above).
                        const double r = 3.0 * symbolScale_;
                        p.drawEllipse(d, r, r);
                    }
                }
                if (clipped) p.restore();
            }
        }
        if (showText_) {
            // Text labels from TX()/TE() (object names, light characters, vertical
            // clearances, …) at constant on-screen size, drawn with a light halo
            // so they stay legible over busy chart fill. Placement follows the
            // S-52 hjust/vjust/xoffs/yoffs the instruction specified; SCAMIN-
            // declutter and the quilt clip apply exactly as for symbols.
            const QColor halo(255, 255, 255, 230);
            QFont f = p.font();
            int curSize = -1;
            for (const BuiltCell* c : order) {
                const auto dcIt = deviceClip.constFind(c->path);
                const bool clipped = (dcIt != deviceClip.constEnd());
                if (clipped) { p.save(); p.setClipPath(*dcIt); }
                const double off = c->drawOffsetX;
                for (const BuiltText& t : c->texts) {
                    if (!scaminPasses(t.scaleMin, scaminDenom)) continue;
                    const QPointF d = cam.map(QPointF(t.pos.x() + off, t.pos.y()));
                    if (!screen.contains(d)) continue;

                    if (int(t.pointSize) != curSize) {
                        curSize = t.pointSize;
                        f.setPointSizeF(curSize); p.setFont(f);
                    }
                    const QFontMetricsF fm(f);
                    const double tw = fm.horizontalAdvance(t.text);
                    const double cw = fm.averageCharWidth();
                    const double th = fm.height();
                    const double px = d.x() + t.xoffs * cw;
                    const double py = d.y() + t.yoffs * th;
                    // Horizontal: 1=centre, 2=right (pivot at right end), 3=left.
                    const double tx = (t.hjust == 2) ? px - tw
                                    : (t.hjust == 3) ? px
                                                     : px - tw / 2.0;
                    // Vertical (to baseline): 1=bottom, 2=centre, 3=top.
                    const double base = (t.vjust == 3) ? py + fm.ascent()
                                      : (t.vjust == 1) ? py - fm.descent()
                                                       : py + fm.ascent() - th / 2.0;
                    const QPointF at(tx, base);
                    // Cheap halo: white at the four neighbours, then ink on top.
                    p.setPen(halo);
                    p.drawText(at + QPointF(-1, 0), t.text);
                    p.drawText(at + QPointF( 1, 0), t.text);
                    p.drawText(at + QPointF( 0,-1), t.text);
                    p.drawText(at + QPointF( 0, 1), t.text);
                    p.setPen(t.color);
                    p.drawText(at, t.text);
                }
                if (clipped) p.restore();
            }
        }
    }

    // 3) Ownship overlay (top of stack).
    drawOwnship(p, cam);

    // 4) Scale bar (lower-right), drawn last so it sits above everything.
    drawScaleBar(p);

    // 5) Plugin overlays, in device coordinates. They use the viewport snapshot
    // for geographic placement and don't know how the canvas is implemented.
    if (!overlays_.empty()) {
        ChartViewport vp;
        vp.sceneToScreen = cam;
        vp.ppm           = ppm_;
        vp.size          = size();
        vp.worldWidthM   = worldWidthM();
        vp.centerSceneX  = scx_;
        p.resetTransform();
        for (IChartOverlay* o : overlays_) o->paint(p, vp);
    }
}

void ChartView::setOwnship(const OwnshipState& s) {
    ownship_ = s;
    // The ownship symbol's freshness follows the position fix specifically.
    ownshipFreshness_ = s.latitudeDeg.freshness;
    // When following, keep the boat centered as it moves. recenterOnOwnship()
    // repaints on success; otherwise repaint here for the symbol's new position.
    if (!(autoFollow_ && recenterOnOwnship())) update();
}

void ChartView::addOverlay(IChartOverlay* overlay) {
    if (overlay && std::find(overlays_.begin(), overlays_.end(), overlay) == overlays_.end()) {
        overlays_.push_back(overlay);
        update();
    }
}

void ChartView::removeOverlay(IChartOverlay* overlay) {
    auto it = std::find(overlays_.begin(), overlays_.end(), overlay);
    if (it != overlays_.end()) { overlays_.erase(it); update(); }
}

void ChartView::setOwnshipPredictionMinutes(double minutes) {
    if (minutes == ownshipPredMin_) return;
    ownshipPredMin_ = minutes;
    update();
}

void ChartView::setHeadingSource(HeadingSource s) {
    if (s == headingSource_) return;
    headingSource_ = s;
    update();
}

void ChartView::drawOwnship(QPainter& p, const QTransform& cam) {
    if (!ownship_.latitudeDeg.valid() || !ownship_.longitudeDeg.valid()) return;

    // Project ownship into the scene, then to the nearest world copy so it shows
    // on-screen even when the user has wrapped across the date line.
    const double sx = proj::lonToX(ownship_.longitudeDeg.value);
    const double sy = -proj::latToY(ownship_.latitudeDeg.value);
    const double off = wrapOffsetFor(sx);
    const QPointF d = cam.map(QPointF(sx + off, sy));

    // Heading for the triangle: use the configured source, falling back to the
    // other when the preferred one has no data so the glyph still has a direction.
    std::optional<double> headingDeg;
    if (headingSource_ == HeadingSource::Cog) {
        if (ownship_.cogDegTrue.valid())          headingDeg = ownship_.cogDegTrue.value;
        else if (ownship_.headingDegTrue.valid()) headingDeg = ownship_.headingDegTrue.value;
    } else {
        if (ownship_.headingDegTrue.valid())  headingDeg = ownship_.headingDegTrue.value;
        else if (ownship_.cogDegTrue.valid()) headingDeg = ownship_.cogDegTrue.value;
    }

    // Red ownship glyph: a simplified boat hull (distinct from the AIS wedges).
    static const vessel::SymbolStyle kOwnship{
        vessel::SymbolStyle::Shape::BoatHull,
        QColor(220, 30, 30),        // fill
        QColor(200, 110, 110, 200), // stale fill
        QColor(40, 0, 0),           // edge
        QColor(40, 0, 0),           // stale edge
        QColor(20, 20, 20, 220)     // pred line
    };
    // Draw the ownship a touch larger than the AIS targets (which use the same
    // vesselScale_) so the boat stands out as the vessel you're on.
    constexpr double kOwnshipScale = 1.15;
    vessel::drawSymbol(p, d, headingDeg, ownship_.sogKnots.valueOr(0.0),
                       ownshipPredMin_, ppm_,
                       ownshipFreshness_ == NavFreshness::Stale, kOwnship,
                       vesselScale_ * kOwnshipScale);
}

// A vertical scale bar in the lower-right corner. Five segments alternating
// filled/hollow; 0 at the bottom, the total at the top. The total is a "nice"
// 1-2-5 number of the chosen distance unit, sized so the bar is at most about a
// third of the view width. Mercator stretches north-south with latitude, so the
// ground distance per pixel is taken at the screen-center latitude as specified.
void ChartView::drawScaleBar(QPainter& p) {
    if (ppm_ <= 0.0 || width() <= 0 || height() <= 0) return;

    // Ground metres per pixel at the center latitude: scene metres are true
    // ground metres at the equator and Mercator scales them by 1/cos(lat).
    const double latC = proj::yToLat(-scy_);
    const double cosLat = std::cos(latC * proj::kDeg2Rad);
    if (cosLat <= 1e-6) return;                 // give up very near the poles
    const double mPerPx = cosLat / ppm_;

    // Distance unit: metres per unit and a short label.
    double unitM = 1852.0;
    QString suffix = QStringLiteral("nm");
    switch (distanceUnit_) {
        case DistanceUnit::StatuteMiles:  unitM = 1609.344; suffix = QStringLiteral("mi"); break;
        case DistanceUnit::Kilometers:    unitM = 1000.0;   suffix = QStringLiteral("km"); break;
        case DistanceUnit::NauticalMiles: break;            // defaults above
    }

    // Target length ~1/3 of the view width, but never taller than the view.
    const double margin = 14.0;
    const double maxBarPx = height() - 2.0 * margin - 26.0;   // leave room for labels
    const double targetPx = std::min(width() / 3.0, maxBarPx);
    if (targetPx < 24.0) return;                // too small to be meaningful

    const double targetUnits = targetPx * mPerPx / unitM;
    if (!(targetUnits > 0.0) || !std::isfinite(targetUnits)) return;

    // Largest 1-2-5 x 10^n value not exceeding the target.
    const double p10 = std::pow(10.0, std::floor(std::log10(targetUnits)));
    const double m = targetUnits / p10;
    const double niceUnits = (m >= 5.0 ? 5.0 : m >= 2.0 ? 2.0 : 1.0) * p10;

    const double barPx = niceUnits * unitM / mPerPx;
    if (!(barPx > 0.0) || !std::isfinite(barPx)) return;

    // Labels.
    const QString topLabel = (niceUnits < 1.0 ? QString::number(niceUnits, 'g', 2)
                                              : QString::number(niceUnits, 'f', 0))
                             + QStringLiteral(" ") + suffix;
    const QString botLabel = QStringLiteral("0");

    p.save();
    p.resetTransform();
    p.setRenderHint(QPainter::Antialiasing, true);

    QFont f = p.font();
    f.setPointSizeF(9.0);
    p.setFont(f);
    QFontMetricsF fm(f);
    const double labelW = std::max(fm.horizontalAdvance(topLabel),
                                   fm.horizontalAdvance(botLabel));

    // Geometry, laid out from the right edge: [panel [labels] gap [bar]] margin.
    const double barThick = 9.0;
    const double gap = 6.0;
    const double barRight = width() - margin;
    const double barLeft  = barRight - barThick;
    const double bottomY  = height() - margin;
    const double topY     = bottomY - barPx;

    // Faint backing panel so the bar reads over any chart colour.
    const double pad = 6.0;
    const QRectF panel(barLeft - gap - labelW - pad, topY - fm.height() / 2.0 - pad,
                       (barRight - (barLeft - gap - labelW)) + 2.0 * pad,
                       (bottomY - topY) + fm.height() + 2.0 * pad);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 190));
    p.drawRoundedRect(panel, 5.0, 5.0);

    // Five alternating segments, filled at the bottom (0) end.
    const double segPx = barPx / 5.0;
    QPen edge(QColor(0, 0, 0));
    edge.setWidthF(1.2);
    p.setPen(edge);
    for (int i = 0; i < 5; ++i) {
        const double y0 = bottomY - (i + 1) * segPx;
        const QRectF seg(barLeft, y0, barThick, segPx);
        p.setBrush((i % 2 == 0) ? QBrush(QColor(0, 0, 0)) : QBrush(Qt::white));
        p.drawRect(seg);
    }

    // End labels, right-aligned against the bar and vertically centered on
    // each end.
    p.setPen(QColor(0, 0, 0));
    const double textRight = barLeft - gap;
    const double textLeft  = textRight - labelW;
    const double lineH = fm.height();
    p.drawText(QRectF(textLeft, topY - lineH / 2.0, labelW, lineH),
               Qt::AlignRight | Qt::AlignVCenter, topLabel);
    p.drawText(QRectF(textLeft, bottomY - lineH / 2.0, labelW, lineH),
               Qt::AlignRight | Qt::AlignVCenter, botLabel);

    p.restore();
}

// ---- input -----------------------------------------------------------------

// Touch-friendly zoom (the +/- buttons). Anchored at the screen centre — there
// is no cursor on touch — and otherwise mirrors the wheel handler so the
// whole-globe floor and auto-follow recentering behave consistently.
void ChartView::zoomBy(double factor) {
    if (ppm_ <= 0.0) return;
    const double target = std::clamp(ppm_ * factor, minPpm(), 1e6);
    if (target == ppm_) return;
    userInteracted_ = true;
    ppm_ = target;       // anchored at centre => scx_/scy_ unchanged
    normalizeCenter();
    if (autoFollow_) recenterOnOwnship();
    beginInteraction();
    emit chartInteracted();   // zoom dismisses transient popups
    updatePointLOD();
    scheduleUpdate();
    update();
}

// Place the +/- buttons in the lower-right corner with a fixed margin that
// keeps them clear of the scale bar (which draws at the same baseline).
void ChartView::positionZoomButtons() {
    if (!zoomInBtn_ || !zoomOutBtn_) return;
    constexpr int kBtnSize    = 48;
    constexpr int kBtnGap     = 8;
    constexpr int kBottomPad  = 14;
    constexpr int kScaleBarPx = 110;   // reserved width for the scale bar
    const int y = height() - kBottomPad - kBtnSize;
    const int inX  = width() - kScaleBarPx - kBtnSize;            // (+) right of (-)
    const int outX = inX - kBtnGap - kBtnSize;
    zoomOutBtn_->move(outX, y);
    zoomInBtn_->move(inX,  y);
    zoomOutBtn_->raise();
    zoomInBtn_->raise();
}

void ChartView::wheelEvent(QWheelEvent* e) {
    if (ppm_ <= 0.0) { e->ignore(); return; }
    userInteracted_ = true;
    const double step = 1.15;
    const double factor = (e->angleDelta().y() > 0) ? step : 1.0 / step;
    // Clamp zoom-out at the whole-globe view (no wraparound tiling) and keep a
    // sane zoom-in ceiling. Clamping rather than ignoring lets a coarse wheel
    // step settle exactly on the floor instead of stopping short of it.
    const double target = std::clamp(ppm_ * factor, minPpm(), 1e6);
    if (target == ppm_) { e->accept(); return; }

    // Keep the scene point under the cursor fixed.
    const QPointF cur = e->position();
    const QPointF under = screenToScene(cur);
    ppm_ = target;
    scx_ = under.x() - (cur.x() - width() / 2.0) / ppm_;
    scy_ = under.y() - (cur.y() - height() / 2.0) / ppm_;
    normalizeCenter();

    // Zooming must not disable follow; while following, keep the boat centered
    // through the zoom instead of anchoring on the cursor.
    if (autoFollow_) recenterOnOwnship();

    beginInteraction();
    emit chartInteracted();   // zoom dismisses transient popups
    updatePointLOD();
    scheduleUpdate();
    update();
    e->accept();
}

void ChartView::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && ppm_ > 0.0) {
        // Offer the gesture to the active editor first. If it grabs (e.g. the
        // press landed on a draggable route node), we drag the node instead of
        // panning the chart.
        if (editor_ && editor_->onPress(e->position())) {
            editorGrab_ = true;
            userInteracted_ = true;
            QWidget::mousePressEvent(e);
            return;
        }
        dragging_ = true;
        panDismissEmitted_ = false;           // re-arm pan dismissal for this gesture
        lastDragPos_ = e->position();
        pressPos_   = e->position();          // for click vs drag at release
        userInteracted_ = true;
        setCursor(Qt::ClosedHandCursor);
        // Arm the long-press timer; cancelled in mouseMove (large motion) or
        // mouseRelease (early release). Editor sessions are exempt above.
        longPressFired_ = false;
        if (!editor_ && longPressTimer_) longPressTimer_->start();
    }
    QWidget::mousePressEvent(e);
}

void ChartView::mouseMoveEvent(QMouseEvent* e) {
    if (ppm_ > 0.0) {
        const QPointF s = screenToScene(e->position());
        emit cursorMoved(proj::xToLon(s.x()), proj::yToLat(-s.y()));
        if (editorGrab_) {                    // dragging a node, not panning
            editor_->onMove(e->position());
            update();
            QWidget::mouseMoveEvent(e);
            return;
        }
        if (dragging_) {
            const QPointF d = e->position() - lastDragPos_;
            lastDragPos_ = e->position();
            // Once the gesture is a real pan (past the click threshold), tell
            // listeners — but only once per drag, and not for tiny click jitter.
            if (!panDismissEmitted_ &&
                (e->position() - pressPos_).manhattanLength() > 4.0) {
                panDismissEmitted_ = true;
                emit chartInteracted();
            }
            // A long-press needs the finger to stay (mostly) put. Cancel as soon
            // as the gesture turns into a pan so a slow drag doesn't trigger it.
            if (longPressTimer_ && longPressTimer_->isActive()
                && (e->position() - pressPos_).manhattanLength() > 8.0)
                longPressTimer_->stop();
            if (autoFollow_) setAutoFollow(false);   // a pan breaks the lock
            scx_ -= d.x() / ppm_;
            scy_ -= d.y() / ppm_;
            normalizeCenter();
            beginInteraction();
            scheduleUpdate();
            update();
        }
    }
    QWidget::mouseMoveEvent(e);
}

void ChartView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && editorGrab_) {
        editorGrab_ = false;
        editor_->onRelease(e->position());
        update();
        QWidget::mouseReleaseEvent(e);
        return;
    }
    if (e->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        setCursor(Qt::OpenHandCursor);
        if (longPressTimer_) longPressTimer_->stop();
        // Release with little movement is a click — unless a long-press already
        // fired, in which case the gesture is consumed and we suppress the click.
        // Otherwise the click is offered to overlays in reverse z-order; the
        // first to consume it (e.g. AIS / route hit) wins. A click that no
        // overlay consumes is an empty-space click and dismisses transient popups.
        if (longPressFired_) {
            longPressFired_ = false;
        } else if ((e->position() - pressPos_).manhattanLength() <= 4.0) {
            bool consumed = false;
            for (auto it = overlays_.rbegin(); it != overlays_.rend(); ++it) {
                if ((*it)->hitTest(e->position())) { consumed = true; break; }
            }
            // Route/AIS overlays get first refusal (above). If none claimed the
            // click, query the chart objects under it; objects found open an info
            // window, otherwise it's an empty-space click that dismisses popups.
            if (!consumed) {
                QList<ChartObjectInfo> objs = pickObjects(e->position());
                if (!objs.isEmpty())
                    emit objectsPicked(objs, e->globalPosition().toPoint());
                else
                    emit chartInteracted();
            }
        }
    }
    QWidget::mouseReleaseEvent(e);
}

void ChartView::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (haveCatalog_ && !userInteracted_) fitToCatalog();
    else if (!haveCatalog_ && !userInteracted_ && rasterSceneBounds_.valid())
        fitToSceneBox(rasterSceneBounds_);   // raster-only folder
    else { updatePointLOD(); scheduleUpdate(); }
    ensureViewForBasemap();   // in case the widget had no size when basemap loaded
    // Widening the window raises the whole-globe floor; re-enforce it so we
    // never sit below the new minimum and tile the world.
    if (ppm_ > 0.0 && ppm_ < minPpm()) ppm_ = minPpm();
    positionZoomButtons();
    maybeBuildBasemap();
    update();
}
