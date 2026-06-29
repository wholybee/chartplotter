#pragma once
#include <QWidget>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QPainterPath>
#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QTransform>
#include <QThreadPool>
#include <QPixmap>
#include <QImage>
#include <QVector>
#include <vector>
#include <utility>
#include <memory>
#include "chart_loader.hpp"
#include "chart_object.hpp"
#include "feature_cache.hpp"
#include "nav_data_store.hpp"   // OwnshipState, NavFreshness
#include "units.hpp"            // DepthUnit
#include "heading_source.hpp"   // HeadingSource
#include "plugin_api.hpp"       // IChartOverlay, ChartViewport
#include "sym_atlas.hpp"        // SymAtlas
#include "mbtiles_reader.hpp"   // MbtilesMeta

class ChartCatalog;
class QTimer;
class QPushButton;
class QThread;
class MbtilesService;
class IChartSource;

// A chart-editing controller (e.g. the route/waypoint editor). When one is set,
// ChartView consults it on the raw press/move/release before its own panning, so
// the editor can grab a node and drag it without the gesture turning into a pan.
// onPress returns true to claim the gesture (suppressing the pan); the subsequent
// move/release for that gesture are then delivered to the editor instead. Points
// are device pixels; the editor converts via the viewport it caches as an overlay.
class IChartEditor {
public:
    virtual ~IChartEditor() = default;
    virtual bool onPress(const QPointF& screenPt)   = 0;   // true => claim gesture
    virtual void onMove(const QPointF& screenPt)    = 0;
    virtual void onRelease(const QPointF& screenPt) = 0;
};

// Identity of one raster tile in the cache: which chart (index into the
// discovered list), the pyramid zoom level, and the XYZ tile column/row.
struct RasterTileKey {
    int chart = 0;
    int z = 0;
    int x = 0;
    int y = 0;
    bool operator==(const RasterTileKey& o) const {
        return chart == o.chart && z == o.z && x == o.x && y == o.y;
    }
};
inline size_t qHash(const RasterTileKey& k, size_t seed = 0) noexcept {
    return qHashMulti(seed, k.chart, k.z, k.x, k.y);
}

// Result handed back from a worker thread after loading one cell.
struct CellLoadResult {
    QString path;
    std::vector<Feature> features;
    BBox bbox;
    bool ok = false;
    QString error;
};

// One ready-to-draw vector primitive: a clipped, simplified path plus its style.
// Built on a worker thread (QPainterPath/QColor are value types, safe off the
// GUI thread); the UI thread draws it directly through the camera transform.
// Coordinates are scene metres: projected Mercator with Y flipped so north is up.
struct BuiltPath {
    QPainterPath path;
    QRectF bounds;             // path.boundingRect(), for view culling
    double z = 0.0;
    bool   filled = false;     // true: area (use brush); false: line (pen only)
    QColor brush;
    bool   hasPen = false;
    QColor penColor;
    qreal  penWidth = 1.0;
    Qt::PenStyle penStyle = Qt::SolidLine;   // SolidLine / DashLine / DotLine
    bool   isDepthContour = false;
    // S-52 complex symbology resolved at build time, rendered at constant on-
    // screen size in device space (see paintEvent). -1 when absent.
    int    apIndex = -1;       // AP() area-pattern fill (tiled motif)
    int    lcIndex = -1;       // LC() complex line (motif stamped along the path)
    int    scaleMin = 0;       // S-57 SCAMIN (0 = none); paint-time declutter floor
};

// A whole cell, clipped to a region and ready to draw. drawOffsetX shifts it by a
// whole-world width so cells near the date line can be drawn on the far side of
// the 180° seam (longitude wrap-around).
// A single sounding: scene position plus the raw depth in metres (S-57's native
// unit). The label is formatted at paint time from the current depth unit, so
// switching feet/metres is a repaint — no rebuild of the cell geometry.
struct Sounding {
    QPointF pos;            // scene position (Y already flipped north-up)
    double  depthM = 0.0;   // raw depth, metres
    bool    hasDepth = false;
    int     scaleMin = 0;   // S-57 SCAMIN (0 = none); paint-time declutter floor
};

// A resolved symbol: scene position + atlas index + optional rotation.
// symIdx == SymAtlas::kNoSymbol means no atlas entry was found; the renderer
// falls back to the magenta dot used before symbol support was added.
// rotationDeg is the S-57 ORIENT angle (degrees CW from true north); zero for
// upright symbols.  Resolved at cell-build time so paint is just a blit.
struct BuiltSymbol {
    QPointF  pos;
    uint16_t symIdx = SymAtlas::kNoSymbol;
    float    rotationDeg = 0.0f;
    int      scaleMin = 0;   // S-57 SCAMIN (0 = none); paint-time declutter floor
};

// A text label drawn at constant on-screen size next to its object. Resolved at
// cell-build time from a TX()/TE() instruction (or the bare OBJNAM); SCAMIN-
// declutter and the Text toggle apply at paint time, like symbols.
//   hjust 1=centre 2=right 3=left   vjust 1=bottom 2=centre 3=top
// xoffs/yoffs are in nominal character-box units (≈ font width/height).
struct BuiltText {
    QPointF pos;             // scene position of the object the label annotates
    QString text;
    int     scaleMin = 0;    // S-57 SCAMIN (0 = none)
    quint8  hjust = 3, vjust = 2;
    int     xoffs = 0, yoffs = 0;
    QColor  color = QColor(40, 40, 40);
    quint8  pointSize = 8;
};

// A light sector: a coloured arc plus its two dashed limit legs, drawn at
// constant on-screen size around a sectored light. Bearings are directions from
// the light (degrees CW from true north); the lit arc sweeps clockwise
// startDeg→endDeg. Resolved at cell-build time from CS(LIGHTS05); SCAMIN
// declutter and the Symbols toggle apply at paint time, like symbols.
struct BuiltLightSector {
    QPointF pos;             // scene position of the light (Y flipped north-up)
    float   startDeg = 0.0f;
    float   endDeg   = 0.0f;
    float   rangeNm  = 0.0f;
    QColor  color;
    int     scaleMin = 0;    // S-57 SCAMIN (0 = none)
};

struct BuiltCell {
    QString path;
    int  band = 0;
    BBox clipBox;                                          // region (real frame)
    double drawOffsetX = 0.0;                              // scene-X wrap offset
    std::vector<BuiltPath>   paths;                        // sorted by z
    std::vector<Sounding>    soundings;                    // scene pos + depth
    std::vector<BuiltSymbol> symbols;                      // scene pos + sym idx
    std::vector<BuiltText>   texts;                        // scene pos + label
    std::vector<BuiltLightSector> sectors;                 // light sector arcs
};

// Chart canvas with a camera-based renderer.
//
// The view is a camera: a center point in projected Mercator metres (scene
// coordinates) plus a zoom in pixels-per-metre. Geometry is painted directly
// with QPainter through the camera transform, rather than via QGraphicsScene.
// This makes panning truly unbounded and — because Mercator longitude is linear
// in X with period worldWidth — lets us wrap at the 180° seam by drawing cells
// shifted by whole-world widths. It also leaves a clean place to draw a
// worldwide basemap underlay beneath the cells.
class ChartView : public QWidget {
    Q_OBJECT
public:
    explicit ChartView(QWidget* parent = nullptr);
    ~ChartView() override;

    void setCatalog(ChartCatalog* catalog);
    // Select the vector-chart backend for cell loads. nullptr (default) uses the
    // built-in ENC/S-57 reader (chart::loadCellFeatures); a non-null IChartSource
    // (e.g. a CM93 plugin) services loads via its loadCell(). Set on the UI
    // thread by MainWindow before each scan, alongside ChartCatalog::setSource.
    void setChartSource(IChartSource* src) { chartSource_ = src; }
    // Called when a chart source is being torn down (plugin shutdown). If it is
    // the active source, in-flight loadCell() workers are drained before the
    // source object dies, and the pipeline falls back to the built-in reader.
    void onChartSourceUnregistered(IChartSource* src);
    void fitToCatalog();
    // Zoom/pan so the most detailed charts in the set fill the screen. Unlike
    // fitToCatalog (which frames the whole set, including the wide small-scale
    // overview cell), this fits the finest navigational band present.
    void zoomToCharts();
    // Recenter on the ownship without changing zoom. No-op if no fix is shown.
    void centerOnOwnship();
    // Auto-follow: keep the view centered on the ownship as it moves. Panning
    // turns it off (zooming does not). No-op-until-data if no fix yet.
    void setAutoFollow(bool on);
    bool autoFollow() const { return autoFollow_; }

    // Coalesced repaint request for data-driven updates (GPS fixes, AIS target
    // changes, route/nav changes, plugin overlays). Unlike update(), which
    // schedules a paint on every call, this batches all requests arriving within
    // a short window into a single repaint, so a fast NMEA/AIS feed can't drive
    // the chart's paint rate (the dominant source of idle CPU). Interactive
    // pan/zoom paths still call update() directly for an immediate frame.
    void requestRepaint();

    // Chart display settings (driven by the core Settings object).
    void setShowSoundings(bool on);
    void setShowSymbols(bool on);
    void setShowText(bool on);   // object name (OBJNAM) labels
    void setShowDepthContours(bool on);
    // When true, soundings/symbols are skipped during a pan/zoom gesture (faster
    // moving frame); when false they stay visible while interacting.
    void setHideSymbolsWhilePanning(bool on);
    // Show/hide the MBTiles raster-chart layer (tracking + loading continue).
    void setShowRasterCharts(bool on);
    // Vector-overlay mode: suppress the opaque chart base — land/water area fills
    // plus the GSHHG basemap — so the raster layer shows through, while keeping
    // all vector linework, depth contours, symbols, soundings and text on top.
    // Intended for an imagery MBTiles base (e.g. satellite). Repaint only.
    void setVectorOverlay(bool on);

    // Point the raster-chart layer at one or more folders: each is scanned
    // (recursively, on a worker thread) for *.mbtiles files, which are drawn
    // beneath the ENC vector cells. Pass the active chart-set folders. Empty
    // clears the layer.
    void setRasterChartFolders(const QStringList& dirs);
    // Detail-level bias, in fractional bands. 0 = nominal mapping from visible
    // width to band; positive pulls in higher-detail cells (more detail on
    // screen); negative backs off. Range -2.0..+2.0.
    void setChartDetailLevel(double level);
    // SCAMIN bias for point-object decluttering, in [-1, +1]. 0 = honour each
    // object's SCAMIN at the current zoom; positive reveals more objects (as if
    // zoomed in further); negative hides more; -1 hides all point objects and
    // +1 shows them all regardless of SCAMIN. Repaints (no rebuild).
    void setChartScaminLevel(double level);
    // Symbol scale factor. 1.0 = nominal (baked) size; range 0.5 .. 3.0.
    void setSymbolScale(double scale);
    // Vessel glyph scale factor (ownship + AIS). 1.0 = nominal; range 0.5..3.0.
    void setVesselScale(double scale);
    void setDepthUnit(DepthUnit u);   // relabels soundings (repaint, no rebuild)
    void setDistanceUnit(DistanceUnit u);   // scale-bar units (repaint)

    // Folder holding GSHHG data (containing GSHHS_shp/). Empty triggers a search
    // of the standard install locations. Loads the basemap underlay async.
    void setBasemapDirectory(const QString& dir);

    // Ownship overlay: the view subscribes to a NavDataStore and draws the
    // ownship symbol over the chart, with appearance reflecting freshness.
    void setOwnship(const OwnshipState& s);   // freshness read per-value from s
    // Length of the course-prediction line, in minutes of run-time at SOG.
    void setOwnshipPredictionMinutes(double minutes);
    // Which direction the ownship glyph points (heading vs COG).
    void setHeadingSource(HeadingSource s);

    // Plugin chart overlays: drawn on top of the chart each frame, in registration
    // order. The view does not own them (the plugin does).
    void addOverlay(IChartOverlay* overlay);
    void removeOverlay(IChartOverlay* overlay);

    // Active chart editor (nullptr = none). While set, press/move/release are
    // offered to it before panning, so it can drag chart nodes. Not owned.
    void setChartEditor(IChartEditor* editor) { editor_ = editor; }

    // Pan/zoom the view to frame a geographic box (degrees), with a little
    // padding. Used to jump to a route when editing it.
    void fitToGeoBox(double latMin, double lonMin, double latMax, double lonMax);

    // Restore the view (center in degrees + zoom) on the next catalog load
    // instead of fitting. One-shot: consumed on the next load.
    void setInitialView(double lon, double lat, double scale);
    // Capture the current pan/zoom and restore it on the next catalog load
    // instead of fitting — used when switching chart sets so the view doesn't
    // snap to the new set's extent. No-op if there is no view yet.
    void keepCurrentViewOnNextLoad();
    // Emit viewChanged immediately with the current view (e.g. on app close).
    void persistViewNow();

signals:
    void cursorMoved(double lon, double lat);
    void statusChanged(const QString& text);
    // Touch-friendly long-press (press-and-hold) on the chart. Fires after ~500 ms
    // with the finger held within a few pixels of pressPos; suppressed when an
    // IChartEditor is active so it doesn't fight with route-editing gestures.
    void longPressed(const QPointF& screenPt);
    // Debounced after panning/zooming; carries the view center (degrees) + zoom.
    void viewChanged(double lon, double lat, double scale);
    // Auto-follow turned on/off (e.g. off when the user pans). Lets the menu
    // keep its checkmark in sync.
    void autoFollowChanged(bool on);
    // The user interacted with the chart itself — an empty-space click, a pan,
    // or a zoom. Transient popups (e.g. the AIS quick-info window) listen for
    // this to dismiss themselves. Not emitted when a click hits a chart overlay
    // (e.g. an AIS target), so target clicks keep their own handling.
    void chartInteracted();
    // How many raster (MBTiles) charts were found in the active folder. Lets the
    // main window report folders that hold raster charts but no ENC cells.
    void rasterChartsChanged(int count);
    // An empty-space click (one not consumed by a route/AIS overlay) landed on
    // one or more chart objects. The main window shows a chooser (if several) or
    // a detail window (if one). globalPos anchors the window near the click.
    void objectsPicked(const QList<ChartObjectInfo>& objects, const QPoint& globalPos);

    // To the MBTiles worker thread (queued). Not for external use.
    void rasterSetFolders(const QStringList& dirs, quint64 gen);
    void rasterRequestTile(int chartId, int z, int x, int y, quint64 gen);

protected:
    void paintEvent(QPaintEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private slots:
    void onCatalogFinished(bool ok, const QString& message);
    void scheduleUpdate();
    void updateVisibleCells();
    // MBTiles worker replies (queued).
    void onRasterDiscovered(const QVector<MbtilesMeta>& charts, quint64 gen);
    void onRasterTileReady(int chartId, int z, int x, int y,
                           const QImage& img, quint64 gen);

private:
    void dispatchLoad(const QString& path);
    void onCellLoaded(CellLoadResult r, quint64 gen);
    void dispatchBuild(const QString& path, FeatureCache::FeaturesPtr feats,
                       int band, const BBox& clipBox, double drawOffsetX);
    void onCellBuilt(BuiltCell bc, quint64 gen, double drawOffsetX);
    void storeCell(BuiltCell bc);
    void removeCell(const QString& path);
    void clearAll();
    void updatePointLOD();

    // Basemap (GSHHG land/lakes underlay) -----------------------------------
    void reloadBasemap();          // resolve directory + available tiers
    void ensureViewForBasemap();   // whole-world view when basemap shows w/o charts
    QString desiredTier() const;   // tier matching the current zoom + availability
    void ensureTierForZoom();      // switch to / load the desired tier
    void loadTier(const QString& tier);
    void onTierLoaded(FeatureCache::FeaturesPtr feats, const QString& tier);
    void maybeBuildBasemap();      // rebuild clipped/simplified copies if needed
    void onBasemapBuilt(std::vector<BuiltCell> cells, FeatureCache::FeaturesPtr feats);

    // Identify chart objects within ~30 px of a click, nearest/most-specific
    // first (points, then lines, then areas). Scans the parsed features of the
    // on-screen (active) cells; excludes the base depth/land area canvas.
    QList<ChartObjectInfo> pickObjects(const QPointF& screenPt);

    bool computeViewBoxes(BBox& view, BBox& wanted, BBox& keep, int& target) const;
    // Number of usage bands the quilt reasons about, 1..kMaxBand. ENC uses 1..6
    // (filename digit); CM93 has 8 native scales (Z, A..G) and maps each to its
    // own band so overlapping scales never share one (which would double-draw).
    static constexpr int kMaxBand = 8;
    static int  bandForVisibleWidth(double metres);
    static BBox expandBox(const BBox& b, double frac);
    static BBox shiftX(const BBox& b, double dx);

    // Raster (MBTiles) layer -------------------------------------------------
    void drawRasterCharts(QPainter& p, const QTransform& cam, const QRectF& vis);
    void requestRasterTile(const RasterTileKey& k);
    void fitToSceneBox(const BBox& sceneBox);   // box already in scene frame
    // True once a view exists from either ENC cells or raster charts.
    bool haveContent() const { return haveCatalog_ || !rasterCharts_.isEmpty(); }

    // Camera ----------------------------------------------------------------
    QTransform cameraTransform() const;     // scene metres -> screen pixels
    QPointF screenToScene(const QPointF& screen) const;
    static double worldWidthM();            // scene width of 360° of longitude
    double minPpm() const;                  // most zoomed-out: globe fills width once
    void   normalizeCenter();               // wrap center X into [-W, W)
    double wrapOffsetFor(double cellCenterX) const;  // nearest whole-world shift
    void   restoreView(double lon, double lat, double scale);
    bool   currentView(double& lon, double& lat, double& scale) const;
    void   beginInteraction();
    bool   recenterOnOwnship();   // move center to the ownship; false if no fix

    bool soundingVisible() const { return showSoundings_ && pointLodVisible_; }
    bool symbolVisible()   const { return showSymbols_   && pointLodVisible_; }
    bool textVisible()     const { return showText_      && pointLodVisible_; }
    bool contourVisible()  const { return showDepthContours_; }

    // Format a sounding label from raw metres using the current depth unit.
    QString formatSounding(double depthM) const;

    // Minimum on-screen spacing (device px) between drawn soundings, given the
    // label line height. Grows with the detail level so the denser soundings
    // pulled in at higher detail don't pile up; returns 0 (no thinning) at or
    // below nominal detail, keeping level 0 identical to before.
    double soundingMinSpacing(double lineHeightPx) const;

    // Approximate display-scale denominator (e.g. 45000 for ~1:45000) for the
    // current zoom, derived from ground metres per pixel and the screen's
    // physical pixel pitch. Used as the reference scale for SCAMIN tests.
    double displayScaleDenominator() const;
    // SCAMIN visibility test for one point object, given the precomputed
    // effective denominator from scaminEffectiveDenominator(). True => draw it.
    // scaleMin == 0 (no SCAMIN) is always eligible except when fully hidden.
    bool   scaminPasses(int scaleMin, double effectiveDenom) const;
    // The denominator a point object's SCAMIN must meet or exceed to be drawn,
    // folding the user's scaminLevel_ bias into displayScaleDenominator().
    // Returns a sentinel for the hard-off / hard-on slider extremes (see .cpp).
    double scaminEffectiveDenominator() const;

    void drawOwnship(QPainter& p, const QTransform& cam);
    void drawScaleBar(QPainter& p);   // lower-right scale bar, in device pixels

    // Static chart layer (Fix 2: pixmap cache). renderStatic draws the basemap +
    // raster + ENC cells + complex symbology + soundings/symbols/text through the
    // given camera into p; vis is the scene rect to cull against and deviceRect is
    // the painter's device rect (for the constant-on-screen-size point/label culls
    // — the cache pixmap, not the widget). renderStaticCache renders that into the
    // offscreen staticCache_; invalidateChart marks the cache stale and repaints.
    void renderStatic(QPainter& p, const QTransform& cam,
                      const QRectF& vis, const QRectF& deviceRect);
    void renderStaticCache();
    void invalidateChart();
    // Touch-friendly zoom: same step as the wheel, anchored at the screen
    // centre (no cursor on touch devices).
    void zoomBy(double factor);
    void positionZoomButtons();       // place + / - buttons left of the scale bar

    // Camera state (scene metres = projected Mercator, Y flipped north-up).
    double scx_ = 0.0;
    double scy_ = 0.0;
    double ppm_ = 0.0;     // pixels per metre; 0 until a catalog/view is set

    ChartCatalog* catalog_ = nullptr;
    IChartSource* chartSource_ = nullptr;   // null = built-in ENC reader
    QThreadPool   pool_;
    QTimer*       updateTimer_ = nullptr;
    QTimer*       aaTimer_ = nullptr;
    QTimer*       saveTimer_ = nullptr;
    // Repaint governor: coalesces data-driven repaint requests (see
    // requestRepaint()). Single-shot; repaintPending_ guards against restarting
    // it on every incoming message so the rate is capped at the interval.
    QTimer*       repaintTimer_ = nullptr;
    bool          repaintPending_ = false;
    QPushButton*  zoomInBtn_ = nullptr;     // lower-right + button
    QPushButton*  zoomOutBtn_ = nullptr;    // lower-right - button

    QHash<QString, BuiltCell> loaded_;
    QHash<QString, int>       bandByPath_;
    QHash<QString, BBox>      bboxByPath_;
    // Quilting (computed in updateVisibleCells, consumed in paintEvent):
    //   active_   — loaded cells that contribute pixels (finest band in their
    //               region); cells fully covered by finer bands are excluded.
    //   drawClip_ — for a cell only partially covered by finer bands, the scene-
    //               frame (cell-native, no wrap offset) path it may draw within.
    //               Absent ⇒ draw unclipped. Keyed by cell path.
    QSet<QString>             active_;
    QHash<QString, QPainterPath> drawClip_;
    FeatureCache              cache_;
    QSet<QString> inFlight_;     // parse running on a worker
    QSet<QString> building_;     // clip/build running on a worker
    QSet<QString> wanted_;       // last computed wanted set
    quint64       generation_ = 0;

    // Raster (MBTiles) chart layer. The service runs on its own thread; the view
    // caches decoded tiles keyed by (chart, z, x, y) and draws them beneath the
    // ENC vector cells. rasterGen_ rises on each folder change so stale worker
    // replies are dropped. tileNeeded_ is per-frame scratch for LRU eviction.
    QThread*        mbThread_  = nullptr;
    MbtilesService* mbService_ = nullptr;
    QVector<MbtilesMeta>          rasterCharts_;
    BBox                          rasterSceneBounds_;   // union, scene frame
    QHash<RasterTileKey, QPixmap> tileCache_;
    QSet<RasterTileKey>           tileInFlight_;
    QSet<RasterTileKey>           tileAbsent_;
    QSet<RasterTileKey>           tileNeeded_;
    quint64                       rasterGen_ = 0;
    bool                          showRasterCharts_ = true;

    // Basemap state. basemap_ holds the clipped/simplified copies (one per wrap
    // offset) currently drawn beneath the cells. The active tier is chosen by
    // zoom; loaded tiers are cached so re-zooming is instant.
    QString                   basemapDir_;       // user override ("" = search)
    QString                   basemapRoot_;      // resolved root with GSHHS_shp/
    QStringList               availableTiers_;   // tiers present in basemapRoot_
    QString                   basemapTier_;      // active tier ("" = none)
    QString                   tierLoading_;      // tier whose load is in flight
    FeatureCache              tierCache_;        // loaded tiers (LRU, active pinned)
    FeatureCache::FeaturesPtr basemapFeats_;     // active tier's features
    std::vector<BuiltCell>    basemap_;
    BBox    basemapClipBox_;          // region basemap_ was built for (k=0 frame)
    double  basemapBuiltPpm_ = 0.0;   // zoom basemap_ was simplified for
    bool    basemapBuilding_ = false;

    // Static-chart pixmap cache (Fix 2). The basemap+raster+cells+points layer is
    // rendered once into an oversized pixmap (the viewport plus a full-viewport
    // margin on each side, still within the cell keepArea) and blitted beneath the
    // dynamic overlays each frame, so moving overlays (ownship, AIS, routes) and
    // in-margin pans don't re-rasterize the chart. It is re-rendered only when
    // settled and the camera has left the
    // cached region, the zoom/size changed, or staticDirty_ was set by a content
    // change (new cell, toggle, …). Mid-gesture the last cache is blitted shifted
    // (or scaled for zoom) and refreshed on settle.
    QPixmap    staticCache_;
    QTransform cacheCam_;                  // scene -> cache-pixel transform
    double     cacheScx_ = 0.0, cacheScy_ = 0.0, cachePpm_ = -1.0;
    int        cacheW_ = 0, cacheH_ = 0;   // widget size the cache was built for
    int        cacheMX_ = 0, cacheMY_ = 0; // pixel margin each side of the viewport
    bool       staticDirty_ = true;        // cache content needs re-rendering

    // Symbol atlas (prebaked from chartsymbols.xml + rastersymbols-day.png).
    // Loaded once at construction; immutable after that, safe to query from
    // worker threads.  When not loaded, point features fall back to a dot.
    SymAtlas symAtlas_;

    bool   havePendingView_ = false;
    double pendingLon_ = 0.0, pendingLat_ = 0.0, pendingScale_ = 0.0;

    bool haveCatalog_ = false;
    bool interacting_ = false;        // drop antialiasing mid-gesture
    bool pointLodVisible_ = true;     // soundings/symbols shown at this zoom (LOD)
    bool showSoundings_ = true;
    bool showSymbols_ = true;
    bool showText_ = true;
    bool showDepthContours_ = true;
    bool vectorOverlay_ = false;      // hide base land/water fills + basemap so raster shows through
    bool hideSymbolsWhilePanning_ = false;   // skip point overlays during a gesture
    double chartDetailLevel_ = 0.0;   // -2.0..+2.0, biases target band
    double scaminLevel_      = 0.0;   // -1.0..+1.0, biases SCAMIN declutter
    double symbolScale_      = 1.0;   // 0.5..3.0, uniform symbol scale
    double vesselScale_      = 1.0;   // 0.5..3.0, ownship + AIS glyph scale
    DepthUnit depthUnit_ = DepthUnit::Feet;   // how soundings are labelled
    DistanceUnit distanceUnit_ = DistanceUnit::NauticalMiles;   // scale-bar units
    bool userInteracted_ = false;
    bool autoFollow_ = false;                 // keep view centered on ownship
    std::vector<IChartOverlay*> overlays_;    // plugin overlays (not owned)
    IChartEditor* editor_ = nullptr;          // active chart editor (not owned)
    bool    editorGrab_ = false;              // editor claimed the current gesture

    bool    dragging_ = false;
    bool    panDismissEmitted_ = false;   // chartInteracted() fired once per drag
    QPointF lastDragPos_;
    QPointF pressPos_;     // for click vs drag (release with little movement = click)
    QTimer* longPressTimer_ = nullptr;    // single-shot, fires longPressed()
    bool    longPressFired_ = false;      // suppress click-on-release after long-press

    OwnshipState ownship_;
    NavFreshness ownshipFreshness_ = NavFreshness::Invalid;
    double       ownshipPredMin_ = 6.0;   // predictor length (minutes)
    HeadingSource headingSource_ = HeadingSource::Heading;   // ownship glyph direction
};
