#include "chart_view.hpp"
#include "chart_catalog.hpp"
#include "chart_source.hpp"
#include "projection.hpp"
#include "geom_clip.hpp"
#include "vessel_symbol.hpp"
#include "sym_atlas.hpp"
#include "theme.hpp"
#include "mbtiles_service.hpp"
#include "prepared_render.hpp"
#include "render_scene_compiler.hpp"
#include "prepared_render_cache.hpp"
#include "cell_builder.hpp"
#include "chart_quilt.hpp"
#include "gpu_chart_view.hpp"
#include "gpu_batches.hpp"
#include "gpu_log.hpp"
#include "bundle_paths.hpp"
#include "debug_trace.hpp"

#include <QPainter>
#include <QPaintEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
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
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Frame telemetry (docs/performance_fix_plan.md, Step 0.2). Enable/disable with
// QT_LOGGING_RULES="chart.telemetry.debug=true|false".
Q_LOGGING_CATEGORY(lcTelemetry, "chart.telemetry")

namespace {

using geom::clipRingToRect;
using geom::clipPolylineToRect;
using geom::pointInRect;

// How long the GPU layer has to produce its first frame after being shown before
// the view gives up and falls back to the CPU painter. Generous enough to absorb
// a slow cold-start on modest hardware (e.g. a Raspberry Pi compiling shaders on
// first use) without a false fallback; the sea-colour fill keeps the wait from
// reading as black. A dead device that never renders is caught this soon.
constexpr int kGpuWatchdogMs = 3000;

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
    roots << bundlepaths::dataDir() + "/gshhg-shp";
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

// Vertex-merge tolerance (projected metres) for a usage band, ~half a pixel at
// the band's display scale. Band-based so chart cells do not lose detail just
// because the user temporarily zoomed through a coarser view.
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


} // namespace

ChartView::ChartView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);
    setAttribute(Qt::WA_OpaquePaintEvent, true);   // we fill the whole widget

    pool_ = new QThreadPool;   // unparented + manually managed (see ~ChartView)
    pool_->setMaxThreadCount(qBound(2, QThread::idealThreadCount(), 8));
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
        // No unconditional GPU re-base here: syncGpuCamera() re-bases only once
        // float32 error at the camera's distance from the scene origin could
        // become visible, and recomposites the raster around the settled view,
        // so a routine pan/zoom settle is camera-only.
        scheduleFrame();
    });

    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(500);
    connect(saveTimer_, &QTimer::timeout, this, [this] {
        double lon, lat, scale;
        if (currentView(lon, lat, scale)) emit viewChanged(lon, lat, scale);
    });

    // Repaint governor: data-driven repaint requests (ownship fixes, AIS target
    // updates, route/nav changes, plugin overlays) are coalesced here so a fast
    // NMEA/AIS feed drives at most ~16 repaints/sec rather than one full chart
    // re-raster per message — the dominant source of idle CPU. Interactive
    // pan/zoom keep calling update() directly for an immediate frame.
    repaintTimer_ = new QTimer(this);
    repaintTimer_->setSingleShot(true);
    repaintTimer_->setInterval(60);     // ~16 Hz
    connect(repaintTimer_, &QTimer::timeout, this, [this] {
        repaintPending_ = false;
        scheduleFrame();
    });

    // GPU device watchdog: armed once the GPU layer is shown, fires once if the
    // RHI never produced a frame and falls the view back to the CPU painter (see
    // checkGpuWatchdog). Single-shot; VeryCoarse — a few seconds' slack is fine.
    gpuWatchdog_ = new QTimer(this);
    gpuWatchdog_->setSingleShot(true);
    gpuWatchdog_->setInterval(kGpuWatchdogMs);
    gpuWatchdog_->setTimerType(Qt::VeryCoarseTimer);
    connect(gpuWatchdog_, &QTimer::timeout, this, [this] { checkGpuWatchdog(); });

    // Coalesce bursts of decoded raster tiles: during load-in dozens arrive per
    // second, and each refresh is a full static-cache re-render (painter) or a
    // full recomposite + texture upload (GPU). One short single-shot timer folds
    // a burst into a single refresh instead of one per tile.
    rasterTileTimer_ = new QTimer(this);
    rasterTileTimer_->setSingleShot(true);
    rasterTileTimer_->setInterval(75);
    connect(rasterTileTimer_, &QTimer::timeout, this, [this] {
        gpuRasterDirty_ = true;   // ignored by the painter path
        invalidateChart();
    });

    // Frame telemetry dump: once per second, and only when at least one counter
    // is nonzero — a truly idle app must log nothing at all. VeryCoarse keeps
    // the timer itself from forcing precise CPU wakeups.
    telemetryTimer_ = new QTimer(this);
    telemetryTimer_->setInterval(1000);
    telemetryTimer_->setTimerType(Qt::VeryCoarseTimer);
    connect(telemetryTimer_, &QTimer::timeout, this, [this] {
        int rhiFrames = 0, cellUploads = 0, texUploads = 0;
        quint64 vertsDrawn = 0;
        if (gpuLayer_)
            gpuLayer_->takeTelemetry(rhiFrames, cellUploads, texUploads, vertsDrawn);
        FrameTelemetry& t = telemetry_;
        if (!t.any() && !rhiFrames && !cellUploads && !texUploads) return;
        qCDebug(lcTelemetry).nospace()
            << "paints=" << t.paints
            << " overlay=" << t.overlayPaints
            << " gpuFrames=" << t.gpuFrames
            << " rhiFrames=" << rhiFrames
            << " requests=" << t.repaintRequests
            << " drawlist=" << t.drawListUpdates
            << " rasterComp=" << t.rasterComposites << '/' << t.rasterCompositeMs << "ms"
            << " symbology=" << t.symbologyPasses << '/' << t.symbologyMs << "ms"
            << " staticRender=" << t.staticRenders << '/' << t.staticRenderMs << "ms"
            << " uploads=" << cellUploads << '+' << texUploads
            << " verts=" << vertsDrawn;
        t = FrameTelemetry{};
    });
    telemetryTimer_->start();

    // Touch zoom buttons (lower-right, just left of the scale bar). Styled as a
    // single vertically-connected pill: (+) on top with rounded top corners, (−)
    // on the bottom with rounded bottom corners, sharing one divider line where
    // they meet. Translucent so they sit nicely over the chart. Auto-repeat lets
    // the user hold to keep zooming on a phone/tablet. Colours follow the OS
    // theme so the glyph stays readable on both light and dark systems.
    const theme::OverlayBtnPalette& ob = theme::overlayBtn();
    auto makeZoomBtn = [this, &ob](const QString& glyph, bool top) {
        auto* b = new QPushButton(glyph, this);
        b->setFixedSize(48, 46);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus);   // taps shouldn't steal keyboard focus
        b->setAutoRepeat(true);
        b->setAutoRepeatDelay(350);
        b->setAutoRepeatInterval(110);
        // Only the outer end of each half is rounded; the shared seam stays
        // square so the two halves read as one continuous pill. The (−) glyph
        // renders low in its box, so the bottom half gets a small bottom padding
        // to lift it back to the optical centre.
        const QString radii = top
            ? QStringLiteral("border-top-left-radius:24px; border-top-right-radius:24px;"
                             " border-bottom-left-radius:0; border-bottom-right-radius:0;")
            : QStringLiteral("border-bottom-left-radius:24px; border-bottom-right-radius:24px;"
                             " border-top-left-radius:0; border-top-right-radius:0;"
                             " padding-bottom:6px;");
        b->setStyleSheet(QStringLiteral(
            "QPushButton{ font-size:26px; font-weight:600; color:%1;"
            " border:1px solid %2; %5 background:%3; }"
            "QPushButton:pressed{ background:%4; }")
            .arg(ob.fg, ob.border, ob.bg, ob.pressed, radii));
        return b;
    };
    constexpr double kZoomStep = 1.15;
    zoomOutBtn_ = makeZoomBtn(QStringLiteral("−"), /*top=*/false);
    zoomInBtn_  = makeZoomBtn(QStringLiteral("+"), /*top=*/true);
    connect(zoomOutBtn_, &QPushButton::clicked, this, [this] { zoomBy(1.0 / kZoomStep); });
    connect(zoomInBtn_,  &QPushButton::clicked, this, [this] { zoomBy(kZoomStep); });
    zoomOutBtn_->show();
    zoomInBtn_->show();
    positionZoomButtons();

    // Load the symbol atlas from the standard data locations.
    // Search order mirrors the GDAL-data and basemap resolver patterns:
    //   1. the bundled data dir (next to the exe, or Contents/Resources on macOS)
    //   2. CHARTPLOTTER_SOURCE_DIR/data/ (in-tree development builds)
    auto tryLoadAtlas = [this](const QString& dir) {
        return symAtlas_.load(dir + QStringLiteral("/symbols.bin"),
                              dir + QStringLiteral("/rastersymbols-day.png"));
    };
    if (!tryLoadAtlas(bundlepaths::dataDir())) {
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
    connect(this, &ChartView::rasterSetFolders,  mbService_, &MbtilesService::setFolders);
    connect(this, &ChartView::rasterRequestTile, mbService_, &MbtilesService::requestTile);
    connect(mbService_, &MbtilesService::discovered, this, &ChartView::onRasterDiscovered);
    connect(mbService_, &MbtilesService::tileReady,  this, &ChartView::onRasterTileReady);
    connect(mbService_, &MbtilesService::message,    this,
            [this](const QString& t) { emit statusChanged(t); });
    mbThread_->start();
}

ChartView::~ChartView() {
    hmvtrace::mark("~ChartView begin");
    // Drop queued tasks and bump the generation so any late result is ignored.
    // Worker lambdas capture only value copies (never `this`), so a task still
    // running here cannot touch this half-destroyed view.
    ++generation_;
    pool_->clear();
    // Give running tasks a short grace period to finish cleanly (a normal cell
    // parse/build completes well within this). If one is wedged — a pathological
    // GDAL parse or a plugin loadCell that never returns — do NOT block the app's
    // exit on it: abandon the pool (deliberately leaked) and let process exit
    // reclaim the thread. A value-member pool would waitForDone() unconditionally
    // in its destructor, which is exactly what left the window-less process alive.
    if (pool_->waitForDone(3000)) {
        hmvtrace::mark("~ChartView pool drained");
        delete pool_;
    } else {
        hmvtrace::mark(QStringLiteral("~ChartView pool WEDGED after 3s (active=%1); "
                                      "abandoning to allow exit")
                           .arg(pool_->activeThreadCount()));
        // intentionally not deleted: its destructor would hang on the stuck task
    }
    pool_ = nullptr;
    // Stop the worker before tearing down: once the thread is joined no code runs
    // there, so deleting the service from this (GUI) thread is race-free.
    if (mbThread_) { mbThread_->quit(); mbThread_->wait(); }
    hmvtrace::mark("~ChartView mbThread joined");
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
    // Course-up: rotate the scene so the chosen bearing points to the top. The
    // rotation sits between the centre translate and the zoom so it pivots about
    // the view centre. (Derivation: with scene +y = south, a rotate of -upDeg
    // maps the up-bearing direction to straight up.) Zero => north-up, no cost.
    if (viewUpDeg_ != 0.0) t.rotate(-viewUpDeg_);
    t.scale(ppm_, ppm_);
    t.translate(-scx_, -scy_);
    return t;
}

QPointF ChartView::screenToScene(const QPointF& s) const {
    if (ppm_ <= 0.0) return QPointF();
    // Invert the full camera (incl. any course-up rotation). Identical to the
    // closed-form north-up inverse when unrotated.
    return cameraTransform().inverted().map(s);
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

        // Respect the quilt clip exactly as rendering does: where a finer band
        // overlaps this cell, drawClip_ restricts what it draws. If the click
        // falls outside this cell's drawn region, none of its features are
        // visible there, so skip the whole cell — otherwise a coarse cell hidden
        // under a finer one returns phantom duplicate hits. drawClip_ is in the
        // cell's own (un-wrapped) scene frame: x = projected X, y = scene Y.
        const auto clipIt = drawClip_.constFind(path);
        if (clipIt != drawClip_.constEnd() &&
            !clipIt.value().contains(QPointF(cx, clickScene.y())))
            continue;

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
            info.lon      = proj::wrapLonDeg(proj::xToLon(repX));
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
    // Deduplicate: the same real-world object is frequently charted in several
    // overlapping cells (different usage bands, or duplicate/overlapping cells in
    // one band, which the quilt clip does not separate). Those produce identical
    // candidates; keep only the first (nearest, since the list is sorted). Identity
    // is class + name + kind + position rounded to ~1 m of projected northing/
    // easting, so genuinely distinct objects are never merged.
    QSet<QString> seen;
    constexpr int kMaxObjects = 10;
    for (const Cand& c : cands) {
        const QString id = c.info.objClass + QChar(0x1f) + c.info.name +
                           QChar(0x1f) + QString::number(int(c.info.kind)) +
                           QChar(0x1f) +
                           QString::number(qlonglong(std::llround(c.info.lon * 1e5))) +
                           QChar(0x1f) +
                           QString::number(qlonglong(std::llround(c.info.lat * 1e5)));
        if (seen.contains(id)) continue;   // already have this object
        seen.insert(id);
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
    scheduleFrame();
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
    if (gpuLayer_) {
        const QList<QString> keys = loaded_.keys();
        for (const QString& p : keys) gpuLayer_->removeCell(p);
        gpuDrawListDirty_ = true;
    }
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
    staticDirty_ = true;     // dropped cells: the cached chart is now stale
}

void ChartView::onCatalogFinished(bool ok, const QString&) {
    ++generation_;
    clearAll();
    haveCatalog_ = ok && catalog_ && catalog_->bounds().valid();
    // NB: do NOT reset userInteracted_ here. It was already reset at scan start
    // (setRasterChartFolders), and the raster discovery runs in parallel: if it
    // finished first it may have already restored the saved view (setting
    // userInteracted_), and resetting now would let the auto-fit below clobber it.

    if (!haveCatalog_) {
        emit statusChanged(QString());
        scheduleFrame();
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
    scheduleFrame();
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
    scheduleFrame();
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
    scheduleFrame();
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
    requestRepaint();            // coalesced: follow mode recenters at fix rate
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
    } else if (courseUp_) {
        setCourseUp(false);       // dropping follow (e.g. a pan) drops course-up too
    }
    emit autoFollowChanged(on);
}

// The bearing the ownship glyph points to (deg true), using the configured
// heading source with the same fallback drawOwnship() uses. NaN when neither
// COG nor heading is available yet.
double ChartView::viewUpBearingDeg() const {
    if (headingSource_ == HeadingSource::Cog) {
        if (ownship_.cogDegTrue.valid())          return ownship_.cogDegTrue.value;
        if (ownship_.headingDegTrue.valid())      return ownship_.headingDegTrue.value;
    } else {
        if (ownship_.headingDegTrue.valid())      return ownship_.headingDegTrue.value;
        if (ownship_.cogDegTrue.valid())          return ownship_.cogDegTrue.value;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

void ChartView::updateCourseUpRotation() {
    if (!courseUp_) return;
    const double b = viewUpBearingDeg();
    if (std::isnan(b)) return;   // no course yet: keep the current rotation
    // Quantise to whole degrees so a jittery COG doesn't re-render the static
    // cache / re-cull cells every fix; only act on a real change.
    const double q = std::round(b);
    if (q == viewUpDeg_) return;
    viewUpDeg_ = q;
    // Chart geometry rotates for free via the blit, but the upright symbology
    // (text, soundings, buoys) is counter-rotated by the current angle when the
    // cache is baked, so a new angle means the cache must be re-rendered. Re-cull
    // cells for the rotated viewport box too, then repaint.
    staticDirty_ = true;
    scheduleUpdate();
    scheduleFrame();
    emit viewRotationChanged(viewUpDeg_);
}

void ChartView::setCourseUp(bool on) {
    if (on == courseUp_) return;
    courseUp_ = on;
    // Toggling changes the cache apron size (course-up needs the larger diagonal
    // apron), so the cache must be rebuilt once here; rotation changes afterwards
    // are re-blits only.
    staticDirty_ = true;
    if (on) {
        setAutoFollow(true);         // course-up keeps the boat centered
        updateCourseUpRotation();    // rotate to the current course now, if known
    } else {
        viewUpDeg_ = 0.0;            // back to north-up
        emit viewRotationChanged(viewUpDeg_);
    }
    scheduleUpdate();
    scheduleFrame();
    emit courseUpChanged(on);
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

void ChartView::requestRepaint() {
    // May be called off the GUI thread (e.g. a plugin data callback via
    // CoreApi::requestChartRepaint). QTimer must be touched on its own thread, so
    // marshal there first; the queued call re-enters on the GUI thread.
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this] { requestRepaint(); },
                                  Qt::QueuedConnection);
        return;
    }
    // Coalesce: if a repaint is already scheduled within the current window this
    // request folds into it. The single-shot interval caps the data-driven
    // repaint rate however fast messages arrive. The painter paintEvent and the
    // GPU refreshGpuFrame() both clear the flag, so any frame (including an
    // interactive one) satisfies a pending request.
    telemetry_.repaintRequests++;   // counted before folding: request pressure
    if (repaintPending_) return;
    repaintPending_ = true;
    repaintTimer_->start();
}

// --- IChartRenderer (painter backend) ---------------------------------------
//
// These forward the renderer contract onto ChartView's existing painter-backed
// implementation. A future GPU backend implements the same interface against its
// own state; the shell and overlays are written against IChartRenderer and do
// not change.

void ChartView::setCamera(const ChartCamera& c) {
    restoreView(c.lon, c.lat, c.scale);
}

ChartCamera ChartView::camera() const {
    ChartCamera c;
    currentView(c.lon, c.lat, c.scale);   // leaves defaults if no view yet
    return c;
}

void ChartView::setDisplaySettings(const ChartDisplaySettings& s) {
    // Each setter is a no-op when the value is unchanged, so pushing the whole
    // bundle costs nothing when nothing moved.
    setShowSoundings(s.showSoundings);
    setShowSymbols(s.showSymbols);
    setShowText(s.showText);
    setShowDepthContours(s.showDepthContours);
    setShowRasterCharts(s.showRasterCharts);
    setVectorOverlay(s.vectorOverlay);
    setChartDetailLevel(s.chartDetailLevel);
    setChartScaminLevel(s.scaminLevel);
    setSymbolScale(s.symbolScale);
}

void ChartView::requestRepaint(RepaintReason reason) {
    if (reason == RepaintReason::Immediate) scheduleFrame();
    else                                    requestRepaint();   // coalesced
}

PickResult ChartView::pick(const QPointF& screenPos) {
    PickResult r;
    r.objects = pickObjects(screenPos);
    return r;
}

bool ChartView::computeViewBoxes(BBox& view, BBox& wanted, BBox& keep, int& target) const {
    if (ppm_ <= 0.0 || width() <= 0) return false;
    double halfWpx = width() / 2.0, halfHpx = height() / 2.0;
    // Course-up: the visible region is a rotated rectangle, so widen the axis-
    // aligned box to its bounding box so cells at the rotated corners still load.
    if (viewUpDeg_ != 0.0) {
        const double a = viewUpDeg_ * proj::kDeg2Rad;
        const double c = std::abs(std::cos(a)), s = std::abs(std::sin(a));
        const double hw = halfWpx, hh = halfHpx;
        halfWpx = hw * c + hh * s;
        halfHpx = hw * s + hh * c;
    }
    const double halfW = halfWpx / ppm_;
    const double halfH = halfHpx / ppm_;
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

    // Quilt selection (which cells draw, at what clip, which must be present) is
    // pure, camera-free logic shared with the GPU backend; see chart_quilt.hpp.
    const chartquilt::QuiltResult q = chartquilt::computeQuilt(
        catalog_->cells(), wantedArea, keepArea, target,
        [this](double cx) { return wrapOffsetFor(cx); });
    const int maxBand = q.maxBand;

    // Only dirty the static cache when quilt ownership actually changes. The
    // clipped path geometry itself is bounded to keepArea, so routine pans can
    // change its far offscreen edge without changing which cells own visible
    // pixels; that should update future renders, not force a rerender now.
    QSet<QString> oldClipped, newClipped;
    for (auto it = drawClip_.constBegin(); it != drawClip_.constEnd(); ++it)
        oldClipped.insert(it.key());
    for (auto it = q.drawClip.constBegin(); it != q.drawClip.constEnd(); ++it)
        newClipped.insert(it.key());
    const bool quiltTopologyChanged = (q.active != active_) || (newClipped != oldClipped);
    active_   = q.active;
    drawClip_ = q.drawClip;
    wanted_   = q.wanted;
    if (quiltTopologyChanged) { staticDirty_ = true; gpuDrawListDirty_ = true; }

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
        if (!keep) {
            removeCell(path);
            staticDirty_ = true;
            continue;
        }

        const BBox clipBox = loaded_[path].clipBox;           // real frame
        const BBox wantedRealFrame = shiftX(wantedArea, -off);
        if (!clipBox.contains(wantedRealFrame) && !building_.contains(path)) {
            if (FeatureCache::FeaturesPtr feats = cache_.get(path))
                dispatchBuild(path, feats, band, shiftX(keepArea, -off), off);
        }
    }

    emit statusChanged(QStringLiteral("Bands ≤ %1  ·  %2 cell(s) shown")
                           .arg(maxBand).arg(loaded_.size()));
    scheduleFrame();
}

// ---- async load / build ----------------------------------------------------

void ChartView::dispatchLoad(const QString& path) {
    inFlight_.insert(path);
    const quint64 gen = generation_;
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
    // The parse itself is backend-neutral (see cell_source.hpp): the built-in
    // cache/decoder dance or the plugin loadCell. `src` must outlive the task
    // (guaranteed: onChartSourceUnregistered drains the pool before it dies).
    watcher->setFuture(QtConcurrent::run(pool_, [path, src]() {
        return cellsource::parseCell(path, src);
    }));
}

void ChartView::onChartSourceUnregistered(IChartSource* src) {
    if (chartSource_ != src) return;
    // Drain in-flight loadCell()/build workers before the source object dies:
    // bump the generation so any results that still post back are discarded,
    // drop queued tasks, then wait out the running ones (they hold `src`).
    ++generation_;
    pool_->clear();
    pool_->waitForDone();
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
    const quint64 fp = atlas ? atlas->fingerprint() : 0;
    const bool wantGpu = useGpu_;   // also emit retained GPU batches for this cell

    auto* watcher = new QFutureWatcher<BuiltCell>(this);
    connect(watcher, &QFutureWatcher<BuiltCell>::finished, this,
            [this, watcher, gen, drawOffsetX]() {
        BuiltCell bc = watcher->result();
        watcher->deleteLater();
        onCellBuilt(std::move(bc), gen, drawOffsetX);
    });
    watcher->setFuture(QtConcurrent::run(pool_, [p, feats, band, clipBox, atlas, fp, wantGpu]() {
        // Prepared-render cache (Stage 5): load the portrayal-resolved scene, or
        // compile it once and store. The positional feature-count guard rejects a
        // stale artifact whose feature vector no longer lines up. instantiateCell
        // then clips/simplifies into a BuiltCell without re-running portrayal.
        PreparedCellRender prep;
        if (!(prepared_render_cache::load(p, fp, prep) &&
              prep.featureCount() == feats->size())) {
            prep = scene::compileScene(p, *feats, atlas);
            prepared_render_cache::store(p, fp, prep);
        }
        BuiltCell bc = cellbuilder::instantiateCell(p, *feats, prep, band, clipBox,
                                                    simplifyToleranceM(band));
        // Retained GPU batches from the built (clipped + simplified) geometry
        // (Stage 7 A4). Origin = clip-box centre in the projected frame so the
        // float32 vertices stay near zero; the GPU layer applies the origin per
        // draw through its camera uniform.
        if (wantGpu) {
            bc.gpuOriginX = clipBox.valid() ? (clipBox.minx + clipBox.maxx) / 2.0 : 0.0;
            bc.gpuOriginY = clipBox.valid() ? (clipBox.miny + clipBox.maxy) / 2.0 : 0.0;
            gpubatches::appendBuiltCellFills(bc, bc.gpuOriginX, bc.gpuOriginY,
                                             bc.gpuTris);
            gpubatches::appendCellBatches(*feats, prep, bc, bc.gpuOriginX, bc.gpuOriginY,
                                          bc.gpuTris, bc.gpuLines, bc.gpuContours);
            // The GPU draws fills/lines from the retained buffers just built, so
            // keep only the painter primitives the constant-size symbology pass
            // still strokes with QPainter (AP area patterns / LC complex lines);
            // dropping the rest releases QPainterPath memory the GPU backend
            // would never touch again.
            bc.paths.erase(std::remove_if(bc.paths.begin(), bc.paths.end(),
                                          [](const BuiltPath& bp) {
                                              return bp.apIndex < 0 && bp.lcIndex < 0;
                                          }),
                           bc.paths.end());
        }
        return bc;
    }));
}

void ChartView::onCellBuilt(BuiltCell bc, quint64 gen, double drawOffsetX) {
    building_.remove(bc.path);
    if (gen != generation_) return;
    if (!wanted_.contains(bc.path)) return;
    bc.drawOffsetX = drawOffsetX;
    const QString path = bc.path;
    storeCell(std::move(bc));
    // Hand the retained batches to the GPU layer (uploaded once, CPU copy
    // freed) and refresh the draw order so the new cell joins the frame.
    if (useGpu_ && gpuLayer_) pushCellToGpu(path, loaded_[path]);
    gpuDrawListDirty_ = true;
    emit statusChanged(QStringLiteral("%1 cell(s) shown").arg(loaded_.size()));
    invalidateChart();
}

void ChartView::storeCell(BuiltCell bc) {
    loaded_.insert(bc.path, std::move(bc));   // replaces any existing
}

void ChartView::removeCell(const QString& path) {
    loaded_.remove(path);
    drawClip_.remove(path);
    active_.remove(path);
    if (gpuLayer_) gpuLayer_->removeCell(path);   // frees its GPU buffers
    gpuDrawListDirty_ = true;
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
    invalidateChart();
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
    staticDirty_ = true;     // basemap dropped: cached chart is stale
    basemapClipBox_ = BBox();
    basemapBuiltPpm_ = 0.0;
    if (availableTiers_.isEmpty()) { scheduleFrame(); return; }
    ensureViewForBasemap();   // need a zoom to choose a tier
    ensureTierForZoom();      // load the tier appropriate for the current zoom
    scheduleFrame();
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
    watcher->setFuture(QtConcurrent::run(pool_, [root, t]() -> FeatureCache::FeaturesPtr {
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
    const bool wantGpu = useGpu_;   // also emit retained GPU batches per world-copy

    auto* watcher = new QFutureWatcher<std::vector<BuiltCell>>(this);
    connect(watcher, &QFutureWatcher<std::vector<BuiltCell>>::finished, this,
            [this, watcher, feats]() {
        std::vector<BuiltCell> cells = watcher->result();
        watcher->deleteLater();
        onBasemapBuilt(std::move(cells), feats);
    });
    watcher->setFuture(QtConcurrent::run(pool_, [feats, reqs, tol, wantGpu]() {
        std::vector<BuiltCell> result;
        result.reserve(reqs.size());
        // Basemap fills are intentionally not precompiled: compiling the raw
        // GSHHG tier triangulates huge unclipped polygons and dominated idle CPU
        // in profiles. Instantiate first, then triangulate the clipped +
        // simplified BuiltPath geometry for the GPU copy below.
        PreparedCellRender prep;
        prep.formatVersion = scene::kPreparedRenderFormat;
        prep.hits.resize(feats->size());
        prep.hasHit.assign(feats->size(), 0);
        for (const auto& r : reqs) {
            BuiltCell bc = cellbuilder::instantiateCell(QString(), *feats, prep, 0, r.first, tol);
            bc.drawOffsetX = r.second;
            if (wantGpu) {
                bc.gpuOriginX = r.first.valid() ? (r.first.minx + r.first.maxx) / 2.0 : 0.0;
                bc.gpuOriginY = r.first.valid() ? (r.first.miny + r.first.maxy) / 2.0 : 0.0;
                gpubatches::appendBuiltCellFills(bc, bc.gpuOriginX, bc.gpuOriginY,
                                                 bc.gpuTris);
                gpubatches::appendCellBatches(*feats, prep, bc, bc.gpuOriginX, bc.gpuOriginY,
                                              bc.gpuTris, bc.gpuLines, bc.gpuContours);
                // Basemap features carry no AP/LC symbology, so in GPU mode
                // nothing strokes these painter paths again — drop them all.
                bc.paths.clear();
                bc.paths.shrink_to_fit();
            }
            result.push_back(std::move(bc));
        }
        return result;
    }));
}

void ChartView::onBasemapBuilt(std::vector<BuiltCell> cells, FeatureCache::FeaturesPtr feats) {
    basemapBuilding_ = false;
    if (feats != basemapFeats_) return;     // data was reloaded while building
    // Swap the retained basemap entries in the GPU layer: the world-copy count
    // can change (date-line proximity), so remove the old set before pushing
    // the new one.
    if (gpuLayer_) {
        for (int i = 0; i < basemapGpuCount_; ++i)
            gpuLayer_->removeCell(QStringLiteral("basemap#%1").arg(i));
        basemapGpuCount_ = 0;
    }
    basemap_ = std::move(cells);
    if (useGpu_ && gpuLayer_) {
        for (std::size_t i = 0; i < basemap_.size(); ++i)
            pushCellToGpu(QStringLiteral("basemap#%1").arg(i), basemap_[i]);
        basemapGpuCount_ = static_cast<int>(basemap_.size());
    }
    gpuDrawListDirty_ = true;
    invalidateChart();
}

void ChartView::setShowSoundings(bool on) {
    if (on == showSoundings_) return;
    showSoundings_ = on; invalidateChart();
}
void ChartView::setShowSymbols(bool on) {
    if (on == showSymbols_) return;
    showSymbols_ = on; invalidateChart();
}
void ChartView::setShowText(bool on) {
    if (on == showText_) return;
    showText_ = on; invalidateChart();
}
void ChartView::setShowDepthContours(bool on) {
    if (on == showDepthContours_) return;
    // GPU mode keeps contours in their own retained bucket, so the toggle is a
    // draw-list change — no rebuild.
    showDepthContours_ = on; gpuDrawListDirty_ = true; invalidateChart();
}

void ChartView::setShowRasterCharts(bool on) {
    if (on == showRasterCharts_) return;
    showRasterCharts_ = on; gpuRasterDirty_ = true; invalidateChart();
}

void ChartView::setVectorOverlay(bool on) {
    if (on == vectorOverlay_) return;
    // Affects GPU cell/basemap fills and the raster underlay, so re-assemble.
    vectorOverlay_ = on; gpuDrawListDirty_ = true; invalidateChart();
}

// ---- raster (MBTiles) layer ------------------------------------------------

void ChartView::setRasterChartFolders(const QStringList& dirs) {
    // New generation invalidates any in-flight discovery / tile replies.
    ++rasterGen_;
    rasterCharts_.clear();
    rasterSceneBounds_ = BBox{};
    tileCache_.clear();
    tileInFlight_.clear();
    tileAbsent_.clear();
    // A folder change is a fresh start: allow the next discovery (or the ENC
    // catalog) to frame the charts. Guards against the old folders' view
    // lingering over a new, geographically distant chart set. This is the single
    // per-scan reset of the flag — onCatalogFinished must not reset it again, or
    // it would clobber a saved view that the parallel raster path had restored.
    userInteracted_ = false;
    emit rasterSetFolders(dirs, rasterGen_);
    gpuRasterDirty_ = true;
    invalidateChart();
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
    gpuRasterDirty_ = true;
    invalidateChart();
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
    // Coalesced refresh (rasterTileTimer_): a burst of tile replies produces one
    // recomposite/re-render rather than one per tile.
    if (!rasterTileTimer_->isActive()) rasterTileTimer_->start();
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
    scheduleFrame();
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

// Choose the raster tiles to draw for `vis`: each chart's native pyramid zoom
// from the current scale, cached tiles preferred, missing ones requested, and
// the nearest cached coarser ancestor (sub-rectangled to the tile's footprint)
// standing in so zoom/pan never flashes blank. Also evicts the pixmap cache to
// its working set. Shared by the painter path (drawRasterCharts blits the
// result) and the GPU path (pushGpuRasterTiles turns it into textured quads).
std::vector<ChartView::RasterTileDraw> ChartView::selectRasterTiles(const QRectF& vis) {
    std::vector<RasterTileDraw> draws;
    if (!showRasterCharts_ || rasterCharts_.isEmpty() || ppm_ <= 0.0) return draws;

    constexpr int    kTilePx       = 256;   // logical tile edge, pixels
    constexpr int    kMaxCacheTiles = 384;  // ~working set; older tiles evicted
    const double ww = worldWidthM();
    const double W  = ww * 0.5;

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
                const QRectF dest(sx, sy, span, span);

                auto it = tileCache_.constFind(k);
                if (it != tileCache_.constEnd()) {
                    draws.push_back({ k, dest, QRectF(it.value().rect()) });
                    continue;
                }
                if (!tileAbsent_.contains(k)) requestRasterTile(k);

                // Fallback: nearest cached coarser ancestor, sub-rectangled to
                // this tile's footprint, so the area isn't blank while loading.
                for (int L = 1; z - L >= m.minZoom; ++L) {
                    const int az = z - L;
                    const int ax = tx >> L, ay = ty >> L;
                    const RasterTileKey ak{chartId, az, ax, ay};
                    auto ait = tileCache_.constFind(ak);
                    if (ait == tileCache_.constEnd()) continue;
                    const QPixmap& pm = ait.value();
                    const double cell = double(pm.width()) / (1 << L);
                    const QRectF src((tx - (ax << L)) * cell,
                                     (ty - (ay << L)) * cell, cell, cell);
                    tileNeeded_.insert(ak);   // in use as a stand-in: keep cached
                    draws.push_back({ ak, dest, src });
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
    return draws;
}

// Draw the raster charts in device space: blit the selected tile set.
void ChartView::drawRasterCharts(QPainter& p, const QTransform& cam, const QRectF& vis) {
    if (!showRasterCharts_ || rasterCharts_.isEmpty() || ppm_ <= 0.0) return;
    p.resetTransform();                       // draw with explicit device rects
    p.setRenderHint(QPainter::SmoothPixmapTransform, !interacting_);
    for (const RasterTileDraw& d : selectRasterTiles(vis)) {
        const auto it = tileCache_.constFind(d.key);
        if (it != tileCache_.constEnd())
            p.drawPixmap(cam.mapRect(d.dest), it.value(), d.src);
    }
}

void ChartView::setChartDetailLevel(double level) {
    if (level < -2.0) level = -2.0;
    if (level >  2.0) level =  2.0;
    if (level == chartDetailLevel_) return;
    chartDetailLevel_ = level;
    updatePointLOD();   // symbol visibility threshold depends on detail level
    scheduleUpdate();   // cell band selection also depends on detail level
    invalidateChart();  // bands/point LOD change; tolerance follows zoom, not detail
}

void ChartView::setChartScaminLevel(double level) {
    if (level < -1.0) level = -1.0;
    if (level >  1.0) level =  1.0;
    if (level == scaminLevel_) return;
    scaminLevel_ = level;
    // SCAMIN is filtered entirely at paint time, so a bias change is a repaint —
    // no cell rebuild or band/LOD recompute needed.
    invalidateChart();
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
    invalidateChart();
}

void ChartView::setVesselScale(double scale) {
    if (scale < 0.5) scale = 0.5;
    if (scale > 3.0) scale = 3.0;
    if (scale == vesselScale_) return;
    vesselScale_ = scale;
    scheduleFrame();
}

void ChartView::setDepthUnit(DepthUnit u) {
    if (u == depthUnit_) return;
    depthUnit_ = u; invalidateChart();   // soundings relabelled: rebuild cache
}

void ChartView::setDistanceUnit(DistanceUnit u) {
    if (u == distanceUnit_) return;
    distanceUnit_ = u; scheduleFrame();   // scale bar relabels on repaint
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
    // GPU backend: the retained RHI layer draws the chart and the translucent
    // overlay layer draws the dynamic pass on top; this widget (fully covered)
    // paints nothing. This branch must stay a pure no-op: Qt repaints this
    // parent to composite the translucent overlay, so scheduling any child
    // update() from here makes every frame schedule the next one — a
    // self-sustaining ~60 Hz repaint loop with the app idle. GPU frames are
    // driven from input/data/timer events via refreshGpuFrame() instead. The
    // repaint governor is also left alone here: a composition pass does no
    // frame work, so it must not cancel a pending coalesced repaint.
    //
    // Safety net: fill the sea colour rather than leaving this a bare no-op. The
    // RHI child composites the chart over this area, but should its first frame
    // ever lag the parent's first composite (a compositor race on show), an
    // unfilled WA_OpaquePaintEvent surface reads as black. Painting the sea makes
    // any such gap read as empty water instead. This is pure fill — it must NOT
    // schedule a child update() (Qt repaints this parent to composite the
    // translucent overlay, so a child update() here would self-sustain a repaint
    // loop); GpuChartView::showEvent is what actually guarantees the first frame.
    if (useGpu_ && gpuLayer_) {
        QPainter p(this);
        p.fillRect(rect(), QColor(204, 224, 242));
        return;
    }

    // This frame satisfies any pending coalesced repaint request; cancel the
    // governor so a data update arriving mid pan/zoom doesn't fire a second,
    // redundant paint right after this one.
    repaintPending_ = false;
    if (repaintTimer_) repaintTimer_->stop();
    telemetry_.paints++;

    QPainter p(this);
    p.fillRect(rect(), QColor(204, 224, 242));

    if (ppm_ <= 0.0) {     // no view established yet (no charts, no basemap)
        p.setPen(QColor(80, 80, 80));
        QFont f = p.font(); f.setPointSize(13); p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("Tap the menu button (top-left) to open a chart folder."));
        return;
    }

    // Serve the static chart from the offscreen cache. Re-render only when
    // settled: mid-gesture we blit the last cache (shifted, or scaled for a
    // zoom) and let the settle frame (aaTimer_) refresh it, so a pan or zoom
    // never pays the full chart raster per frame.
    if (!staticCacheReusable() && !interacting_) renderStaticCache();
    blitStaticCache(p);

    // Dynamic overlays at the live camera, composited over the cached chart every
    // frame (these move independently of the chart, so they are never cached).
    paintDynamic(p);
}

// The dynamic pass: ownship, scale bar, and plugin overlays at the live camera.
// Shared by the painter path (onto this widget, over the cached chart) and the
// GPU path (onto the translucent overlay layer, over the RHI surface). Both draw
// with the same camera and widget size, so placement is identical.
void ChartView::paintDynamic(QPainter& p) {
    if (ppm_ <= 0.0) return;
    const QTransform cam = cameraTransform();
    p.setRenderHint(QPainter::Antialiasing, true);   // smooth the moving overlays

    // GPU mode: the RHI layer draws only area fills + simple lines, so the
    // constant-size chart symbology (patterns, complex lines, soundings, symbols,
    // text, light sectors) sits here, above the chart but below the ownship/AIS/
    // route overlays. It is served from the same offscreen apron cache the
    // painter path uses — re-rendered only on settle, blitted (shifted, or
    // scaled mid-zoom) otherwise — so a pan/zoom frame never re-rasterizes S-52
    // symbology (the Stage 7 pan-smoothness regression). In GPU mode the cache
    // holds just the symbology over a transparent background (renderStaticCache).
    if (useGpu_) {
        if (!staticCacheReusable() && !interacting_) renderStaticCache();
        blitStaticCache(p);
        p.resetTransform();
        p.setRenderHint(QPainter::Antialiasing, true);
    }

    drawOwnship(p, cam);            // 3) ownship glyph (top of the chart stack)
    drawScaleBar(p);               // 4) scale bar (lower-right)
    // 5) Plugin overlays, in device coordinates. They use the viewport snapshot
    // for geographic placement and don't know how the canvas is implemented.
    if (!overlays_.empty()) {
        ChartViewport vp;
        vp.sceneToScreen = cam;
        vp.ppm           = ppm_;
        vp.size          = size();
        vp.worldWidthM   = worldWidthM();
        vp.centerSceneX  = scx_;
        vp.upDegrees     = viewUpDeg_;
        p.resetTransform();
        for (IChartOverlay* o : overlays_) o->paint(p, vp);
    }
}

// ---- GPU backend (Stage 7 A4) ----------------------------------------------

bool ChartView::eventFilter(QObject* obj, QEvent* e) {
    // The translucent overlay layer delegates its painting back here so the
    // dynamic pass is drawn identically to the painter path.
    if (obj == overlayLayer_ && e->type() == QEvent::Paint) {
        telemetry_.overlayPaints++;
        QPainter p(overlayLayer_);
        paintDynamic(p);
        return true;
    }
    return QWidget::eventFilter(obj, e);
}

void ChartView::setRenderBackend(RenderBackend pref) {
    backendPref_ = pref;
    applyBackend();
}

void ChartView::applyBackend() {
    // Single auto-fallback decision point: resolve the preference against a real
    // RHI-availability probe, so a missing/broken device always lands on the
    // painter and can never blank the chart.
    const bool want = chartrender::resolveUseGpu(backendPref_, GpuChartView::isAvailable());
    if (want == useGpu_) return;   // no backend change
    useGpu_ = want;

    // The offscreen cache holds full-chart content in painter mode but only
    // transparent symbology in GPU mode — wrong for the other backend either
    // way. Drop it rather than keep a stale ~1.5×-viewport pixmap alive.
    staticCache_ = QPixmap();
    staticDirty_ = true;

    if (useGpu_) {
        if (!gpuLayer_) {
            gpuLayer_ = new GpuChartView(this);
            gpuLayer_->setAttribute(Qt::WA_TransparentForMouseEvents, true);  // input -> this
            gpuLayer_->setGeometry(rect());
            // Device loss (RHI recreated after a window/screen change): every
            // retained cell buffer died and the CPU batches were freed at
            // upload, so rebuild the cells and basemap from cached features
            // (parse-free, same as a backend switch).
            connect(gpuLayer_, &GpuChartView::deviceLost, this, [this] {
                if (!useGpu_) return;
                gpuLayer_->clearCells();   // drop survivors from the old generation
                ++generation_;
                loaded_.clear();
                active_.clear();
                drawClip_.clear();
                wanted_.clear();
                inFlight_.clear();
                building_.clear();
                basemapGpuCount_ = 0;
                staticDirty_ = true;
                gpuDrawListDirty_ = true;
                gpuRasterDirty_ = true;
                updateVisibleCells();
                basemapBuiltPpm_ = 0.0;
                maybeBuildBasemap();
                scheduleFrame();
            });
        }
        if (!overlayLayer_) {
            overlayLayer_ = new QWidget(this);
            overlayLayer_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            overlayLayer_->setAttribute(Qt::WA_NoSystemBackground, true);
            overlayLayer_->setAttribute(Qt::WA_TranslucentBackground, true);
            overlayLayer_->setGeometry(rect());
            overlayLayer_->installEventFilter(this);
        }
        gpuLayer_->show();
        gpuLayer_->lower();          // chart layer sits at the bottom of the stack
        overlayLayer_->show();
        overlayLayer_->raise();      // dynamic pass above the chart
        if (zoomInBtn_)  zoomInBtn_->raise();   // buttons stay clickable on top
        if (zoomOutBtn_) zoomOutBtn_->raise();
        // Watch for a dead device. If we're already on screen (a runtime toggle),
        // arm now; if not (the usual startup path, applied before the window is
        // shown), showEvent arms it once the layer can actually render.
        if (isVisible()) armGpuWatchdog();
    } else {
        if (gpuLayer_)     gpuLayer_->hide();
        if (overlayLayer_) overlayLayer_->hide();
        if (gpuWatchdog_)  gpuWatchdog_->stop();   // not in GPU mode; nothing to watch
    }

    // Drop every retained GPU entry (cells + basemap): switching to the painter
    // frees the GPU memory, switching to the GPU starts from a clean slate that
    // the rebuilds below re-populate.
    if (gpuLayer_) gpuLayer_->clearCells();
    basemapGpuCount_ = 0;
    gpuDrawListDirty_ = true;

    // Rebuild the loaded cells for the target backend (GPU batches are emitted by
    // the build worker only when useGpu_). Features stay cached, so this is a
    // parse-free rebuild; the camera and catalog are untouched.
    if (haveCatalog_) {
        ++generation_;
        loaded_.clear();
        active_.clear();
        drawClip_.clear();
        wanted_.clear();
        inFlight_.clear();
        building_.clear();
        staticDirty_   = true;
        updateVisibleCells();
    }
    // Rebuild the basemap so it gains (or drops) its GPU batches for the new
    // backend. Forcing the built-zoom stale triggers a re-instantiation; it is a
    // no-op when no basemap tier is loaded.
    basemapBuiltPpm_ = 0.0;
    maybeBuildBasemap();
    scheduleFrame();
}

void ChartView::pushCellToGpu(const QString& key, BuiltCell& c) {
    if (!gpuLayer_) return;
    // Per-cell vertex budget — the number the clip/simplify batch generation
    // keeps near what the painter actually strokes.
    qCDebug(lcTelemetry).nospace()
        << "gpuCell " << key << ": tris=" << c.gpuTris.size()
        << " lines=" << c.gpuLines.size()
        << " contours=" << c.gpuContours.size();
    // Culling bounds: the clip region covers all emitted geometry (fills are
    // filtered to it, lines clipped to it); shift into the draw frame by the
    // wrap offset. An invalid clip box (never expected) falls back to "never
    // cull".
    double minX = -1e12, minY = -1e12, maxX = 1e12, maxY = 1e12;
    if (c.clipBox.valid()) {
        minX = c.clipBox.minx + c.drawOffsetX;
        maxX = c.clipBox.maxx + c.drawOffsetX;
        minY = c.clipBox.miny;
        maxY = c.clipBox.maxy;
    }
    gpuLayer_->setCell(key, std::move(c.gpuTris), std::move(c.gpuLines),
                       std::move(c.gpuContours),
                       c.gpuOriginX + c.drawOffsetX, c.gpuOriginY,
                       minX, minY, maxX, maxY);
    // The GPU layer owns the data now (and frees it after upload); drop the
    // moved-from shells so the BuiltCell holds no vertex memory.
    c.gpuTris = {};
    c.gpuLines = {};
    c.gpuContours = {};
}

void ChartView::rebuildGpuDrawList() {
    if (!gpuLayer_) return;
    // Basemap world-copies beneath everything — suppressed entirely in
    // vector-overlay mode, where the raster imagery is the opaque base.
    QStringList base;
    if (!vectorOverlay_)
        for (int i = 0; i < basemapGpuCount_; ++i)
            base << QStringLiteral("basemap#%1").arg(i);
    // Active cells coarse-band-first so finer detail overprints, matching the
    // painter. This is a list edit — no vertex copying, no upload.
    std::vector<std::pair<int, QString>> order;
    order.reserve(active_.size());
    for (const QString& path : active_) {
        const auto it = loaded_.constFind(path);
        if (it != loaded_.constEnd()) order.emplace_back(it->band, path);
    }
    std::sort(order.begin(), order.end());
    QStringList cellKeys;
    for (const auto& pr : order) cellKeys << pr.second;
    gpuLayer_->setDrawList(base, cellKeys, !vectorOverlay_, showDepthContours_);
    gpuDrawListDirty_ = false;
    telemetry_.drawListUpdates++;
}

// Pack a raster tile's identity for the GPU texture cache: chart id, zoom, and
// tile coordinates. MBTiles zooms (< 64) and tile coords (< 2^24) fit easily.
static quint64 tileTexId(const RasterTileKey& k) {
    return (quint64(quint32(k.chart)) << 54) |
           (quint64(quint32(k.z) & 0x3F) << 48) |
           (quint64(quint32(k.x) & 0xFFFFFF) << 24) |
           quint64(quint32(k.y) & 0xFFFFFF);
}

void ChartView::pushGpuRasterTiles() {
    if (!gpuLayer_ || ppm_ <= 0.0 || width() <= 0 || height() <= 0) return;
    // Record the camera this selection is for: syncGpuCamera() reselects once
    // the settled view moves/zooms/resizes.
    rasterCompScx_ = scx_;
    rasterCompScy_ = scy_;
    rasterCompPpm_ = ppm_;
    rasterCompW_ = width();
    rasterCompH_ = height();
    if (!showRasterCharts_ || rasterCharts_.isEmpty()) {
        gpuLayer_->setRasterTiles({});
        return;
    }
    QElapsedTimer telemT;
    telemT.start();
    // Select with the painter cache's quarter-viewport apron each side, so
    // mid-gesture pans stay covered until the settle reselection. Tiles the GPU
    // layer already retains cost nothing; only new ones convert and upload.
    const double halfW = ((width()  * 1.5) / 2.0) / ppm_;
    const double halfH = ((height() * 1.5) / 2.0) / ppm_;
    const QRectF vis(scx_ - halfW, scy_ - halfH, 2.0 * halfW, 2.0 * halfH);
    const std::vector<RasterTileDraw> draws = selectRasterTiles(vis);

    std::vector<GpuChartView::TileQuad> quads;
    quads.reserve(draws.size());
    for (const RasterTileDraw& d : draws) {
        const auto it = tileCache_.constFind(d.key);
        if (it == tileCache_.constEnd()) continue;
        const QPixmap& pm = it.value();
        if (pm.width() <= 0 || pm.height() <= 0) continue;
        const quint64 id = tileTexId(d.key);
        if (!gpuLayer_->hasTileTexture(id))
            gpuLayer_->setTileTexture(id, pm.toImage());
        GpuChartView::TileQuad q;
        q.texId = id;
        q.x0 = d.dest.left();
        q.x1 = d.dest.right();
        q.y0 = -d.dest.bottom();   // scene (Y down) -> projected (Y up)
        q.y1 = -d.dest.top();
        q.u0 = static_cast<float>(d.src.left()   / pm.width());
        q.u1 = static_cast<float>(d.src.right()  / pm.width());
        q.v0 = static_cast<float>(d.src.top()    / pm.height());
        q.v1 = static_cast<float>(d.src.bottom() / pm.height());
        quads.push_back(q);
    }
    gpuLayer_->setRasterTiles(std::move(quads));
    telemetry_.rasterComposites++;      // now counts tile (re)selections
    telemetry_.rasterCompositeMs += telemT.elapsed();
}

void ChartView::syncGpuCamera() {
    if (!gpuLayer_) return;
    if (gpuDrawListDirty_) rebuildGpuDrawList();   // cheap list edit, no upload
    // Reselect the raster tile set when tiles changed or the settled view
    // moved/zoomed/resized since the last selection. Selection is cheap and the
    // GPU layer no-op-guards an unchanged set, so this can run on any settled
    // camera change; never mid-gesture — the retained tiles are scene-glued and
    // pan correctly until the settle reselection widens the set.
    const bool rasterStale = !interacting_ && ppm_ > 0.0 &&
        (rasterCompScx_ != scx_ || rasterCompScy_ != scy_ || rasterCompPpm_ != ppm_ ||
         rasterCompW_ != width() || rasterCompH_ != height());
    if (gpuRasterDirty_ || rasterStale) { pushGpuRasterTiles(); gpuRasterDirty_ = false; }
    // Pan/zoom: an absolute-camera uniform update, no geometry rebuild — the
    // retained win. Per-cell origins are applied per draw inside the GPU layer,
    // so float32 precision never needs a scene re-base.
    gpuLayer_->setCamera(scx_, -scy_, ppm_, viewUpDeg_);
}

void ChartView::scheduleFrame() {
    if (useGpu_ && gpuLayer_) refreshGpuFrame();
    else update();
}

void ChartView::refreshGpuFrame() {
    if (!gpuLayer_) return;
    // This frame satisfies any pending coalesced repaint request, exactly as a
    // painter-path paintEvent does (the GPU-mode paintEvent is a no-op and
    // leaves the governor alone).
    repaintPending_ = false;
    if (repaintTimer_) repaintTimer_->stop();
    telemetry_.gpuFrames++;
    syncGpuCamera();
    if (overlayLayer_) overlayLayer_->update();
}

void ChartView::armGpuWatchdog() {
    if (!useGpu_ || !gpuLayer_ || !gpuWatchdog_) return;
    // Just (re)start the one-shot. The check is "did the device ever render a
    // frame", not "did it render more since now" — a healthy device with no
    // charts loaded renders exactly one frame on show and then legitimately
    // idles, so a since-armed delta would falsely condemn it. GpuChartView::
    // showEvent guarantees that one frame on a working device.
    gpuWatchdog_->start();
}

void ChartView::checkGpuWatchdog() {
    if (!useGpu_ || !gpuLayer_) return;   // already on the painter; nothing to judge
    if (gpuLayer_->renderedFrames() > 0)
        return;                            // the device produced a frame — healthy
    // No frame in the whole watchdog window: the RHI device came up dead. This is
    // the random black-screen fault — and because the GPU widget's device is
    // created once and reused, nothing in-process would ever recover it. Fall the
    // whole view back to the CPU painter (which never touches the RHI) and tell
    // the shell so it can show a brief note. The persisted "use GPU" preference is
    // left untouched: the next launch tries the GPU again, in case the driver has
    // since recovered.
    gpulog::write(QStringLiteral("watchdog: no GPU frame in %1 ms - falling back "
                                 "to CPU painter").arg(kGpuWatchdogMs));
    backendPref_ = RenderBackend::Cpu;
    applyBackend();                        // hides the GPU layers, rebuilds for the painter
    emit gpuFellBackToCpu(
        QStringLiteral("GPU acceleration unavailable — using CPU rendering. "
                       "Details logged to %1").arg(gpulog::path()));
}

void ChartView::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    // Startup applies the backend before the top-level window is shown, so the GPU
    // layer couldn't render (or be judged) yet. Now that we're on screen, arm the
    // watchdog; the GPU layer's own showEvent has just forced its first frame.
    if (useGpu_ && gpuLayer_) armGpuWatchdog();
}

// Render the static chart into the offscreen cache at the current camera, with a
// margin around the viewport so small pans blit straight from it. Called only
// when settled (never mid-gesture). Sizes the pixmap to device pixels.
void ChartView::renderStaticCache() {
    const qreal dpr = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    const int W = width(), H = height();
    if (W <= 0 || H <= 0) return;
    QElapsedTimer telemT;
    telemT.start();
    // Quarter-viewport margin each side == 1.5x the viewport, matching the
    // original cache size. Larger aprons reduce blank-edge exposure during long
    // pans, but they multiply every static-cache render by the apron area.
    int mx = W / 4, my = H / 4;
    // Course-up: the cache is rendered north-up and rotated into place by the
    // blit, so a rotation change costs only a re-blit (no re-render). But the
    // rotated viewport sweeps out to the diagonal, so the apron must cover the
    // whole-turn worst case (half the diagonal minus the half-extent) or the
    // screen corners would show blank wedges as the boat turns.
    if (courseUp_) {
        const double diag = std::hypot(double(W), double(H));
        mx = std::max(mx, int(std::ceil((diag - W) / 2.0)));
        my = std::max(my, int(std::ceil((diag - H) / 2.0)));
    }
    cacheMX_ = mx; cacheMY_ = my;
    cacheW_ = W; cacheH_ = H;
    cacheScx_ = scx_; cacheScy_ = scy_; cachePpm_ = ppm_;

    const int pw = W + 2 * mx, ph = H + 2 * my;
    const QSize devSize(int(std::lround(pw * dpr)), int(std::lround(ph * dpr)));
    if (staticCache_.size() != devSize) staticCache_ = QPixmap(devSize);
    staticCache_.setDevicePixelRatio(dpr);
    // Filled per backend below: opaque sea colour under the painter's full
    // chart render, transparent under the GPU mode's symbology-only content.

    // Cache camera: same centre/zoom as the view, but the oversized pixmap's
    // centre maps to the view centre so the margin is symmetric.
    QTransform cc;
    cc.translate(pw / 2.0, ph / 2.0);
    cc.scale(ppm_, ppm_);
    cc.translate(-scx_, -scy_);
    cacheCam_ = cc;

    const QRectF visCache(scx_ - (pw / 2.0) / ppm_, scy_ - (ph / 2.0) / ppm_,
                          pw / ppm_, ph / ppm_);
    if (useGpu_) {
        // GPU mode: the RHI layer draws the chart base (fills, lines, raster),
        // so the cache holds only the constant-size S-52 symbology over a
        // transparent background; the overlay layer blits it above the GPU
        // surface exactly like the painter blits its full-chart cache.
        staticCache_.fill(Qt::transparent);
        QPainter cp(&staticCache_);
        cp.setRenderHint(QPainter::Antialiasing, true);
        QElapsedTimer symT;
        symT.start();
        drawPointSymbology(cp, cc, visCache, QRectF(0, 0, pw, ph));
        telemetry_.symbologyPasses++;
        telemetry_.symbologyMs += symT.elapsed();
    } else {
        staticCache_.fill(QColor(204, 224, 242));
        QPainter cp(&staticCache_);
        renderStatic(cp, cc, visCache, QRectF(0, 0, pw, ph));
    }
    staticDirty_ = false;
    telemetry_.staticRenders++;
    telemetry_.staticRenderMs += telemT.elapsed();
}

bool ChartView::staticCacheReusable() const {
    // Measure the pan since the cache was built along the shortest path around the
    // 180° seam: normalizeCenter() wraps scx_ by a whole world-width when it
    // crosses, so the raw (scx_ - cacheScx_) would jump by ~ww and needlessly
    // (and incorrectly) reject a cache that blitStaticCache() can still place.
    const double ww = worldWidthM();
    double dx = scx_ - cacheScx_;
    dx -= std::round(dx / ww) * ww;
    const double dxPx = dx * ppm_;
    const double dyPx = (scy_ - cacheScy_) * ppm_;
    return !staticDirty_ && !staticCache_.isNull()
           && ppm_ == cachePpm_ && cacheW_ == width() && cacheH_ == height()
           && std::abs(dxPx) <= cacheMX_ && std::abs(dyPx) <= cacheMY_;
}

void ChartView::blitStaticCache(QPainter& p) {
    if (staticCache_.isNull()) return;
    // Map cache-pixel space to the live camera. Equal zoom -> pure translation
    // (crisp); different zoom (mid-gesture placeholder) -> scaled blit. Across the
    // 180° seam the live centre has wrapped a whole world-width away from the
    // cache's; shift the cache by that offset (the same nearest-copy rule the
    // cells use) so it lands on-screen instead of a world off it — the source of
    // the blank chart when panning through the date line.
    QTransform wrap;
    wrap.translate(wrapOffsetFor(cacheScx_), 0.0);
    const QTransform blit = cacheCam_.inverted() * wrap * cameraTransform();
    p.save();
    // Smooth when the blit isn't an axis-aligned 1:1 copy: a zoom placeholder or
    // a course-up rotation both resample the cached pixmap.
    p.setRenderHint(QPainter::SmoothPixmapTransform, ppm_ != cachePpm_ || viewUpDeg_ != 0.0);
    p.setTransform(blit);
    p.drawPixmap(0, 0, staticCache_);
    p.restore();
}

void ChartView::invalidateChart() {
    staticDirty_ = true;
    scheduleFrame();
}

// Draw one light sector around the light at device point `c`: the two dashed
// limit legs out to radius R, then the coloured arc at R. Bearings in `s` are
// directions from the light (deg CW from true north); the lit arc sweeps
// clockwise s.startDeg→s.endDeg. The scene is north-up with device Y pointing
// down, so a bearing b maps to the unit vector (sin b, -cos b). The arc is a
// sampled polyline, which sidesteps Qt's drawArc angle/Y-down conventions.
static void drawLightSector(QPainter& p, const QPointF& c,
                            const BuiltLightSector& s, double R) {
    constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
    auto dir = [&](double bearingDeg) -> QPointF {
        const double r = bearingDeg * kDeg2Rad;
        return QPointF(std::sin(r), -std::cos(r));
    };
    const double sweep = s.endDeg - s.startDeg;   // > 0, clockwise

    // Limit legs: thin dashed dark-grey lines to each sector boundary.
    QPen legPen(QColor(70, 70, 70, 200));
    legPen.setWidthF(1.0);
    legPen.setStyle(Qt::DashLine);
    p.setPen(legPen);
    p.setBrush(Qt::NoBrush);
    p.drawLine(c, c + dir(s.startDeg) * R);
    p.drawLine(c, c + dir(s.endDeg)   * R);

    // Coloured arc at radius R, ~3° per segment.
    const int steps = std::max(2, int(std::ceil(sweep / 3.0)));
    QPolygonF arc;
    arc.reserve(steps + 1);
    for (int i = 0; i <= steps; ++i)
        arc << c + dir(s.startDeg + sweep * (double(i) / steps)) * R;
    QPen arcPen(s.color);
    arcPen.setWidthF(2.5);
    arcPen.setCapStyle(Qt::RoundCap);
    arcPen.setJoinStyle(Qt::RoundJoin);
    p.setPen(arcPen);
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(arc);
}

// The static chart layer. Formerly the body of paintEvent; now it always renders
// the settled look (antialiased, point overlays shown) because the result is
// cached and blitted during gestures rather than re-rendered per frame. `cam`,
// `vis` and `deviceRect` are supplied by renderStaticCache (oversized for the
// margin) instead of being read from the widget.
void ChartView::renderStatic(QPainter& p, const QTransform& cam,
                             const QRectF& vis, const QRectF& deviceRect) {
    p.setRenderHint(QPainter::Antialiasing, true);

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

    // Constant-on-screen-size symbology (patterns, complex lines, soundings,
    // symbols, text, light sectors). Factored out so the GPU overlay can run the
    // same passes over its RHI fills; here it renders into the static cache.
    drawPointSymbology(p, cam, vis, deviceRect);
}

// Constant-on-screen-size chart symbology in device space: S-52 area patterns
// (AP) + complex lines (LC), then soundings, light sectors, symbols, and text.
// Extracted from renderStatic (Stage 7 A5) so the GPU backend can draw the same
// point/label passes on the translucent overlay layer above its RHI fills — they
// are constant-size regardless of backend, so QPainter is the pragmatic renderer.
void ChartView::drawPointSymbology(QPainter& p, const QTransform& cam,
                                   const QRectF& vis, const QRectF& deviceRect) {
    // Cell draw order (coarse band first), as in the fill pass.
    std::vector<const BuiltCell*> order;
    order.reserve(loaded_.size());
    for (auto it = loaded_.constBegin(); it != loaded_.constEnd(); ++it)
        order.push_back(&it.value());
    std::sort(order.begin(), order.end(),
              [](const BuiltCell* a, const BuiltCell* b) { return a->band < b->band; });

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
    //
    // Gated like the point overlays below: hidden once the zoom passes the
    // point-LOD threshold (pointLodVisible_) and per-feature by SCAMIN. Without
    // this the constant-size motifs (cables, ferry routes, etc.) keep drawing
    // when zoomed out, cluttering the display and overshooting the shrunken
    // feature endpoints. scaminDenom is shared with the sounding/symbol pass.
    const double scaminDenom = scaminEffectiveDenominator();
    if (symAtlas_.isLoaded() && pointLodVisible_) {
        p.resetTransform();   // device-space clips/anchors below
        const QRectF visDev = deviceRect.adjusted(-48, -48, 48, 48);
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
                if (!scaminPasses(bp.scaleMin, scaminDenom)) continue;
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
    // Point overlays show when the zoom allows them (pointLodVisible_). They are
    // baked into the cache at the settled look and blitted during gestures, so
    // there is no per-frame point cost to gate on interaction here.
    if (pointLodVisible_) {
        const QRectF screen = deviceRect.adjusted(-24, -24, 24, 24);

        // Cross-cell duplicate suppression. The same point object is often charted
        // in several overlapping cells; the quilt clip separates different bands
        // but not duplicate/overlapping cells within one band, so identical symbols
        // and labels would stamp on top of each other (most visible on text). Each
        // point pass keeps a set of (quantized scene position, content) keys and
        // skips a repeat. True duplicates share an exact scene position, so 0.5 m
        // buckets never merge genuinely distinct objects. Keyed in scene metres so
        // it is stable under pan/zoom.
        auto sceneKey = [](double sx, double sy, quint64 content) -> quint64 {
            auto mix = [](quint64 h, quint64 v) {
                v += 0x9E3779B97F4A7C15ULL;
                h ^= v + (h << 6) + (h >> 2);
                return h;
            };
            quint64 h = mix(0, quint64(qint64(std::llround(sx * 2.0))));
            h = mix(h, quint64(qint64(std::llround(sy * 2.0))));
            return mix(h, content);
        };

        // SCAMIN declutter threshold (computed above, shared with the LC/AP
        // pass): point objects whose SCAMIN is smaller than this are dropped.
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
            std::unordered_set<quint64> seen;

            for (const BuiltCell* c : order) {
                const auto dcIt = deviceClip.constFind(c->path);
                const bool clipped = (dcIt != deviceClip.constEnd());
                if (clipped) { p.save(); p.setClipPath(*dcIt); }
                const double off = c->drawOffsetX;
                for (const Sounding& s : c->soundings) {
                    if (!scaminPasses(s.scaleMin, scaminDenom)) continue;
                    const QPointF d = cam.map(QPointF(s.pos.x() + off, s.pos.y()));
                    if (!screen.contains(d)) continue;
                    // A clipped-away instance must not claim the dedup slot, or it
                    // would suppress the visible copy in the finer cell on top.
                    if (clipped && !dcIt->contains(d)) continue;
                    // Suppress the same sounding charted in an overlapping cell
                    // (independent of the detail-level thinning below).
                    if (!seen.insert(sceneKey(s.pos.x() + off, s.pos.y(),
                                              quint64(qRound(s.depthM * 10.0)))).second)
                        continue;
                    if (!farEnough(d)) continue;
                    const QString text = s.hasDepth ? formatSounding(s.depthM)
                                                    : QStringLiteral(".");
                    // Course-up: the whole cache is blitted rotated, so counter-
                    // rotate the label about its point to keep the number upright.
                    if (viewUpDeg_ != 0.0) {
                        p.save();
                        p.translate(d); p.rotate(viewUpDeg_);
                        p.drawText(QPointF(1.0, asc), text);
                        p.restore();
                    } else {
                        p.drawText(QPointF(d.x() + 1.0, d.y() + asc), text);
                    }
                    remember(d);
                }
                if (clipped) p.restore();
            }
        }
        if (showSymbols_) {
            // Light sectors: coloured arcs + dashed limit legs around sectored
            // lights, at constant on-screen size. Drawn before the flares (the
            // symbol pass below) so the light symbol sits on top of its arcs.
            const double R = 60.0 * symbolScale_;   // arc radius, device px
            const QRectF sectorScreen = screen.adjusted(-R, -R, R, R);
            std::unordered_set<quint64> seen;
            for (const BuiltCell* c : order) {
                if (c->sectors.empty()) continue;
                const auto dcIt = deviceClip.constFind(c->path);
                const bool clipped = (dcIt != deviceClip.constEnd());
                if (clipped) { p.save(); p.setClipPath(*dcIt); }
                const double off = c->drawOffsetX;
                for (const BuiltLightSector& s : c->sectors) {
                    if (!scaminPasses(s.scaleMin, scaminDenom)) continue;
                    const QPointF d = cam.map(QPointF(s.pos.x() + off, s.pos.y()));
                    if (!sectorScreen.contains(d)) continue;
                    // A clipped-away instance must not claim the dedup slot, or it
                    // would suppress the visible copy in the finer cell on top.
                    if (clipped && !dcIt->contains(d)) continue;
                    const quint64 content = (quint64(qRound(s.startDeg)) << 32) ^
                                            (quint64(qRound(s.endDeg)) << 16) ^
                                            quint64(s.color.rgb());
                    if (!seen.insert(sceneKey(s.pos.x() + off, s.pos.y(), content)).second)
                        continue;
                    drawLightSector(p, d, s, R);
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
            std::unordered_set<quint64> seen;
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
                    // A clipped-away instance must not claim the dedup slot, or it
                    // would suppress the visible copy in the finer cell on top.
                    if (clipped && !dcIt->contains(d)) continue;
                    const quint64 content = (quint64(sym.symIdx) << 16) ^
                                            quint64(quint16(qRound(sym.rotationDeg)));
                    if (!seen.insert(sceneKey(sym.pos.x() + off, sym.pos.y(),
                                              content)).second)
                        continue;

                    if (atlasOk && sym.symIdx != SymAtlas::kNoSymbol) {
                        // Course-up (cache blitted rotated): an upright symbol
                        // (no baked orientation, e.g. a buoy) is counter-rotated
                        // by the view angle so it stays upright; an oriented
                        // symbol (arrow/topmark with a real-world ORIENT) keeps
                        // its bearing and so rotates with the chart.
                        double drawRot = sym.rotationDeg;
                        if (viewUpDeg_ != 0.0 && sym.rotationDeg == 0.0)
                            drawRot = viewUpDeg_;
                        symAtlas_.draw(p, sym.symIdx, d, drawRot,
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
            // Font-only metrics, recomputed solely when the point size changes.
            // Previously a QFontMetricsF was constructed for every label (and
            // ascent/descent/height/avgWidth re-queried per glyph); only the
            // per-text horizontalAdvance actually depends on the string.
            QFontMetricsF fm(f);
            double cw = fm.averageCharWidth(), th = fm.height(),
                   asc = fm.ascent(), desc = fm.descent();
            std::unordered_set<quint64> seen;
            for (const BuiltCell* c : order) {
                const auto dcIt = deviceClip.constFind(c->path);
                const bool clipped = (dcIt != deviceClip.constEnd());
                if (clipped) { p.save(); p.setClipPath(*dcIt); }
                const double off = c->drawOffsetX;
                for (const BuiltText& t : c->texts) {
                    if (!scaminPasses(t.scaleMin, scaminDenom)) continue;
                    const QPointF d = cam.map(QPointF(t.pos.x() + off, t.pos.y()));
                    if (!screen.contains(d)) continue;
                    // A clipped-away instance must not claim the dedup slot, or it
                    // would suppress the visible copy in the finer cell on top.
                    if (clipped && !dcIt->contains(d)) continue;
                    if (!seen.insert(sceneKey(t.pos.x() + off, t.pos.y(),
                                              quint64(qHash(t.text)))).second)
                        continue;   // same label already drawn from another cell

                    if (int(t.pointSize) != curSize) {
                        curSize = t.pointSize;
                        f.setPointSizeF(curSize); p.setFont(f);
                        fm = QFontMetricsF(f);
                        cw = fm.averageCharWidth();
                        th = fm.height();
                        asc = fm.ascent();
                        desc = fm.descent();
                    }
                    const double tw = fm.horizontalAdvance(t.text);   // string-dependent
                    const double px = d.x() + t.xoffs * cw;
                    const double py = d.y() + t.yoffs * th;
                    // Horizontal: 1=centre, 2=right (pivot at right end), 3=left.
                    const double tx = (t.hjust == 2) ? px - tw
                                    : (t.hjust == 3) ? px
                                                     : px - tw / 2.0;
                    // Vertical (to baseline): 1=bottom, 2=centre, 3=top.
                    const double base = (t.vjust == 3) ? py + asc
                                      : (t.vjust == 1) ? py - desc
                                                       : py + asc - th / 2.0;
                    const QPointF at(tx, base);
                    // Cheap halo: white at the four neighbours, then ink on top.
                    // Course-up (cache blitted rotated): counter-rotate the label
                    // about its anchor so text stays horizontal and readable.
                    const bool rot = (viewUpDeg_ != 0.0);
                    if (rot) { p.save(); p.translate(d); p.rotate(viewUpDeg_); }
                    const QPointF a = rot ? at - d : at;
                    p.setPen(halo);
                    p.drawText(a + QPointF(-1, 0), t.text);
                    p.drawText(a + QPointF( 1, 0), t.text);
                    p.drawText(a + QPointF( 0,-1), t.text);
                    p.drawText(a + QPointF( 0, 1), t.text);
                    p.setPen(t.color);
                    p.drawText(a, t.text);
                    if (rot) p.restore();
                }
                if (clipped) p.restore();
            }
        }
    }
}

void ChartView::setOwnship(const OwnshipState& s) {
    ownship_ = s;
    // The ownship symbol's freshness follows the position fix specifically.
    ownshipFreshness_ = s.latitudeDeg.freshness;
    // Course-up: track the new course before recentering so the rotation and the
    // recenter fold into one coalesced repaint.
    if (courseUp_) updateCourseUpRotation();
    // When following, keep the boat centered as it moves. recenterOnOwnship()
    // repaints on success; otherwise repaint here for the symbol's new position.
    // Coalesced: ownship fixes arrive at the GPS rate, so this must not force a
    // full chart re-raster per fix.
    if (!(autoFollow_ && recenterOnOwnship())) requestRepaint();
}

void ChartView::addOverlay(IChartOverlay* overlay) {
    if (overlay && std::find(overlays_.begin(), overlays_.end(), overlay) == overlays_.end()) {
        overlays_.push_back(overlay);
        scheduleFrame();
    }
}

void ChartView::removeOverlay(IChartOverlay* overlay) {
    auto it = std::find(overlays_.begin(), overlays_.end(), overlay);
    if (it != overlays_.end()) { overlays_.erase(it); scheduleFrame(); }
}

void ChartView::setOwnshipPredictionMinutes(double minutes) {
    if (minutes == ownshipPredMin_) return;
    ownshipPredMin_ = minutes;
    scheduleFrame();
}

void ChartView::setHeadingSource(HeadingSource s) {
    if (s == headingSource_) return;
    headingSource_ = s;
    scheduleFrame();
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
    // drawSymbol orients the glyph clockwise from screen-up; in course-up the top
    // of the screen is viewUpDeg_, so subtract it to keep the boat pointing true.
    if (headingDeg && viewUpDeg_ != 0.0) headingDeg = *headingDeg - viewUpDeg_;

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
    scheduleFrame();
}

// Place the +/- buttons in the lower-right corner as a single vertical pill:
// (+) on top, (−) below it, with a fixed margin that keeps them clear of the
// scale bar (which draws at the same baseline). The two halves overlap by 1px so
// their borders merge into one divider line rather than doubling up.
void ChartView::positionZoomButtons() {
    if (!zoomInBtn_ || !zoomOutBtn_) return;
    constexpr int kBtnW       = 48;
    constexpr int kBtnH       = 46;
    constexpr int kSeam       = 1;     // overlap so the shared edge is a single line
    constexpr int kBottomPad  = 14;
    constexpr int kScaleBarPx = 110;   // reserved width for the scale bar
    const int x    = width() - kScaleBarPx - kBtnW;
    const int outY = height() - kBottomPad - kBtnH;              // (−) on the bottom
    const int inY  = outY - kBtnH + kSeam;                       // (+) stacked on top
    zoomInBtn_->move(x, inY);
    zoomOutBtn_->move(x, outY);
    zoomInBtn_->raise();
    zoomOutBtn_->raise();   // bottom half on top so its border draws the divider
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
    scheduleFrame();
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
        emit cursorMoved(proj::wrapLonDeg(proj::xToLon(s.x())), proj::yToLat(-s.y()));
        if (editorGrab_) {                    // dragging a node, not panning
            editor_->onMove(e->position());
            scheduleFrame();
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
            scheduleFrame();
        }
    }
    QWidget::mouseMoveEvent(e);
}

void ChartView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && editorGrab_) {
        editorGrab_ = false;
        editor_->onRelease(e->position());
        scheduleFrame();
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
    if (gpuLayer_)     gpuLayer_->setGeometry(rect());
    if (overlayLayer_) overlayLayer_->setGeometry(rect());
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
    scheduleFrame();
}
