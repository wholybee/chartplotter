#include "main_window.hpp"
#include "app_info.hpp"
#include "debug_trace.hpp"
#include "about_dialog.hpp"
#include "window_dragger.hpp"
#include "chart_view.hpp"
#include "chart_catalog.hpp"
#include "theme.hpp"
#include "settings.hpp"
#include "side_menu.hpp"
#include "chart_sets_dialog.hpp"
#include "prepare_cache_dialog.hpp"
#include "units_dialog.hpp"
#include "navigation_options_dialog.hpp"
#include "stale_thresholds_dialog.hpp"
#include "ownship_prediction_dialog.hpp"
#include "nav_data_browser_window.hpp"
#include "data_priority_dialog.hpp"
#include "chart_detail_dialog.hpp"
#include "chart_symbol_size_dialog.hpp"
#include "ship_size_dialog.hpp"
#include "ownship_mmsi_dialog.hpp"
#include "heading_source_dialog.hpp"
#include "dangerous_ships_dialog.hpp"
#include "ais_target_list_dialog.hpp"
#include "route_store.hpp"
#include "route_navigator.hpp"
#include "nav_display_window.hpp"
#include "route_overlay.hpp"
#include "route_waypoint_dialog.hpp"
#include "route_properties_dialog.hpp"
#include "waypoint_properties_dialog.hpp"
#include "route_quick_info_window.hpp"
#include "chart_object_info_window.hpp"
#include "layers_dialog.hpp"
#include "name_dialog.hpp"
#include "nav_data_store.hpp"
#include "ais_target_store.hpp"
#include "units.hpp"
#include "cpa_calculator.hpp"
#include "ais_overlay.hpp"
#include "ais_alarm.hpp"
#include "ais_target_info_window.hpp"
#include "ais_quick_info_window.hpp"
#include "core_api.hpp"
#include "plugin_manager.hpp"
#include "nmea0183_plugin.hpp"
#include "nmea2000_plugin.hpp"
#include "bundle_paths.hpp"

#include <QCoreApplication>
#include <QStatusBar>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QMenu>
#include <QAction>
#include <QMessageBox>
#include <QDialog>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QScroller>
#include <QFrame>
#include <QTimer>
#include <QScreen>
#include <QFileDialog>
#include <QDir>
#include <QStringList>
#include <QEvent>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QSettings>
#include <QCursor>
#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <QPolygonF>
#include <QColor>
#include <algorithm>
#include <cmath>

namespace {
// Paint the "layers" glyph — three stacked sheets (diamonds), the top one
// filled — tinted to `c` so it matches the button's themed foreground. Drawn in
// code rather than a font glyph so it renders identically on every platform.
QPixmap makeLayersIcon(const QColor& c, int px) {
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c);
    pen.setWidthF(std::max(1.5, px * 0.06));
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);

    const double mid = px / 2.0;
    const double hw  = px * 0.34;   // sheet half-width
    const double hh  = px * 0.17;   // sheet half-height
    const double gap = px * 0.19;   // vertical spacing between sheets
    auto sheet = [&](double cy) {
        QPolygonF d;
        d << QPointF(mid, cy - hh) << QPointF(mid + hw, cy)
          << QPointF(mid, cy + hh) << QPointF(mid - hw, cy);
        return d;
    };
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(sheet(mid + gap));   // bottom sheet (outline)
    p.drawPolygon(sheet(mid));         // middle sheet (outline)
    p.setBrush(c);                     // top sheet filled = the active layer
    p.drawPolygon(sheet(mid - gap));
    return pm;
}

// Paint a compass-needle glyph for the course-up toggle: a two-tone diamond
// needle (red north half, `c`-tinted south half). `c` is the themed foreground
// so it reads on the button; callers pass white for the active (checked) state.
// `rotDeg` turns the needle clockwise so it can point to true north as the chart
// rotates (0 = needle up).
QPixmap makeCompassIcon(const QColor& c, int px, double rotDeg = 0.0) {
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    const double mid = px / 2.0;
    p.translate(mid, mid);
    if (rotDeg != 0.0) p.rotate(rotDeg);
    const double hw  = px * 0.17;   // needle half-width at the waist
    const double tip = px * 0.34;   // centre-to-tip length
    QPainterPath north; north.moveTo(0, -tip);
    north.lineTo(-hw, 0); north.lineTo(hw, 0); north.closeSubpath();
    QPainterPath south; south.moveTo(0, tip);
    south.lineTo(-hw, 0); south.lineTo(hw, 0); south.closeSubpath();
    p.setBrush(QColor(210, 40, 40)); p.drawPath(north);   // north half (red)
    p.setBrush(c);                   p.drawPath(south);   // south half (themed)
    return pm;
}
} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(appinfo::name());
    resize(1100, 750);   // first-run default; overridden by restoreGeometry below

    // Restore the previous size / position / maximised state (no-op on first run
    // or when the saved screen no longer exists — Qt rejects an off-screen rect).
    const QByteArray geom =
        QSettings().value(QStringLiteral("window/geometry")).toByteArray();
    if (!geom.isEmpty()) restoreGeometry(geom);

    settings_ = new Settings(this);
    // Seed the process-wide coordinate display format so every widget formats
    // lat/lon consistently from the first paint.
    units::setCoordFormat(settings_->angleFormat());

    view_ = new ChartView(this);
    setCentralWidget(view_);
    connect(view_, &ChartView::cursorMoved,   this, &MainWindow::onCursorMoved);
    connect(view_, &ChartView::statusChanged, this, &MainWindow::onViewStatus);

    // Apply persisted display settings, then keep the view in sync with changes.
    view_->setShowSoundings(settings_->showSoundings());
    view_->setShowSymbols(settings_->showSymbols());
    view_->setShowText(settings_->showText());
    view_->setShowDepthContours(settings_->showDepthContours());
    view_->setShowRasterCharts(settings_->showRasterCharts());
    view_->setVectorOverlay(settings_->vectorOverlay());
    connect(settings_, &Settings::showSoundingsChanged,     view_, &ChartView::setShowSoundings);
    connect(settings_, &Settings::showSymbolsChanged,       view_, &ChartView::setShowSymbols);
    connect(settings_, &Settings::showTextChanged,          view_, &ChartView::setShowText);
    connect(settings_, &Settings::showDepthContoursChanged, view_, &ChartView::setShowDepthContours);
    connect(settings_, &Settings::showRasterChartsChanged,  view_, &ChartView::setShowRasterCharts);
    connect(settings_, &Settings::vectorOverlayChanged,     view_, &ChartView::setVectorOverlay);
    // Rendering backend (Stage 7): the "Use GPU acceleration" toggle. Applied now
    // and on change; ChartView resolves it against an RHI probe and falls back to
    // the painter automatically if the GPU device can't be created.
    view_->setRenderBackend(settings_->renderBackend());
    connect(settings_, &Settings::renderBackendChanged,     view_, &ChartView::setRenderBackend);
    connect(view_, &ChartView::rasterChartsChanged, this, &MainWindow::onRasterChartsChanged);
    view_->setChartDetailLevel(settings_->chartDetailLevel());
    connect(settings_, &Settings::chartDetailLevelChanged,
            view_, &ChartView::setChartDetailLevel);
    view_->setChartScaminLevel(settings_->chartScaminLevel());
    connect(settings_, &Settings::chartScaminLevelChanged,
            view_, &ChartView::setChartScaminLevel);
    view_->setSymbolScale(settings_->symbolScale());
    connect(settings_, &Settings::symbolScaleChanged,
            view_, &ChartView::setSymbolScale);
    view_->setVesselScale(settings_->vesselScale());
    connect(settings_, &Settings::vesselScaleChanged,
            view_, &ChartView::setVesselScale);
    view_->setHeadingSource(settings_->headingSource());
    connect(settings_, &Settings::headingSourceChanged,
            view_, &ChartView::setHeadingSource);

    // Depth unit drives how soundings are labelled; distance unit drives the
    // scale bar.
    view_->setDepthUnit(settings_->depthUnit());
    connect(settings_, &Settings::depthUnitChanged, view_, &ChartView::setDepthUnit);
    view_->setDistanceUnit(settings_->distanceUnit());
    connect(settings_, &Settings::distanceUnitChanged, view_, &ChartView::setDistanceUnit);
    // Bearing reference (true/magnetic) drives the route overlay's per-leg heading
    // labels; repaint when it changes. (Distance-unit changes already repaint via
    // ChartView::setDistanceUnit relabelling the scale bar.)
    connect(settings_, &Settings::bearingModeChanged, this,
            [this](BearingMode) { if (view_) view_->requestRepaint(); });

    // Coordinate (lat/lon) display format: update the shared format and refresh
    // the live displays (status-bar cursor + the open routes/waypoints browser).
    // The AIS info and nav-data browser windows refresh on their own 1 Hz timers.
    connect(settings_, &Settings::angleFormatChanged, this, [this](AngleFormat f) {
        units::setCoordFormat(f);
        onCursorMoved(lastCursorLon_, lastCursorLat_);
        if (routeWptDlg_) routeWptDlg_->refreshLists();
    });

    // Ownship course-prediction length (minutes), persisted via Settings.
    view_->setOwnshipPredictionMinutes(settings_->ownshipPredictionMinutes());
    connect(settings_, &Settings::ownshipPredictionMinutesChanged,
            view_, &ChartView::setOwnshipPredictionMinutes);

    // Basemap underlay: load from the configured folder (or a standard location)
    // and keep it in sync if the user picks a different one.
    view_->setBasemapDirectory(settings_->basemapDirectory());
    connect(settings_, &Settings::basemapDirectoryChanged, view_, &ChartView::setBasemapDirectory);

    // Remember the pan/zoom location across runs: the view publishes its location
    // (debounced) and we persist it. Restoring happens only for the startup
    // auto-load below, so explicit chart-set switches still fit to the new set.
    connect(view_, &ChartView::viewChanged, settings_, &Settings::setView);

    catalog_ = new ChartCatalog(this);
    connect(catalog_, &ChartCatalog::progress, this, &MainWindow::onScanProgress);
    connect(catalog_, &ChartCatalog::finished, this, &MainWindow::onScanFinished);
    view_->setCatalog(catalog_);

    // Nav data store: the store owns shared ownship state, the view subscribes,
    // and publishers (NMEA plugins, etc.) feed it through the INavDataPublisher
    // API. This is the foundation for AIS/instruments/routes.
    navStore_  = new NavDataStore(this);
    navStore_->setStaleSeconds(settings_->staleSeconds());
    navStore_->setInvalidSeconds(settings_->invalidSeconds());
    connect(settings_, &Settings::staleThresholdsChanged,
            this, [this](double s, double i) {
        navStore_->setStaleSeconds(s);
        navStore_->setInvalidSeconds(i);
    });
    // Source arbitration: highest-priority source wins, falling back when its
    // data goes invalid. Sources register into registry_ (e.g. NMEA via its
    // plugin); the saved order is applied after plugin init (below).
    connect(settings_, &Settings::dataSourcePriorityChanged,
            navStore_, &NavDataStore::setSourcePriority);
    // ownshipChanged fires on new data and on any per-value freshness transition.
    connect(navStore_, &NavDataStore::ownshipChanged, this, &MainWindow::publishOwnshipToView);

    // AIS target store: keyed by MMSI, fed by the NMEA 0183 plugin's AIS decoder
    // via IAisPublisher; consumers subscribe to it. Stale at 6 min, lost at 12.
    aisStore_ = new AisTargetStore(this);

    // AIS chart overlay: green vessel glyphs (same shape as ownship, predictor
    // line + cancellation slash when stale). Kept in step with ownship's
    // configurable predictor length, and triggers a repaint as targets change.
    aisOverlay_ = std::make_unique<AisOverlay>(aisStore_, navStore_);
    aisOverlay_->setPredictionMinutes(settings_->ownshipPredictionMinutes());
    aisOverlay_->setVesselScale(settings_->vesselScale());
    aisOverlay_->setVisible(settings_->showAisTargets());
    aisStore_->setStaleSeconds(settings_->aisStaleSeconds());
    aisStore_->setLostSeconds(settings_->aisLostSeconds());
    aisOverlay_->setOnTargetClicked([this](quint32 mmsi) { showAisTarget(mmsi); });
    view_->addOverlay(aisOverlay_.get());
    // Any chart interaction (empty click, pan, zoom) dismisses the quick-look
    // popup; the full info window is unaffected.
    connect(view_, &ChartView::chartInteracted, this, [this] {
        if (aisQuickInfo_) aisQuickInfo_->close();
        aisQuickInfoMmsi_ = 0;
        if (routeQuickInfo_) routeQuickInfo_->close();
        if (objectInfo_) objectInfo_->close();
    });
    connect(view_, &ChartView::objectsPicked, this, &MainWindow::onObjectsPicked);
    connect(settings_, &Settings::ownshipPredictionMinutesChanged,
            this, [this](double m) {
        if (aisOverlay_) aisOverlay_->setPredictionMinutes(m);
        if (view_) view_->requestRepaint(RepaintReason::Immediate);
    });
    connect(settings_, &Settings::vesselScaleChanged,
            this, [this](double s) {
        if (aisOverlay_) aisOverlay_->setVesselScale(s);
    });
    connect(settings_, &Settings::showAisTargetsChanged,
            this, [this](bool on) {
        if (aisOverlay_) aisOverlay_->setVisible(on);
        if (view_) view_->requestRepaint(RepaintReason::Immediate);
    });
    // Audible alarm on dangerous targets. Created before the danger-rule sync
    // below so it receives the initial rules/enable state.
    aisAlarm_ = new AisAlarm(aisStore_, this);

    // Dangerous-ship rules: push the current values into the overlay (and the
    // alarm) and keep them in sync; a change repaints so flags update immediately.
    auto applyDangerRules = [this] {
        if (!aisOverlay_) return;
        DangerRules r;
        r.ignoreFarEnabled = settings_->dangerIgnoreFarEnabled();
        r.ignoreFarNm      = settings_->dangerIgnoreFarNm();
        r.cpaEnabled  = settings_->dangerCpaEnabled();
        r.cpaNm       = settings_->dangerCpaNm();
        r.tcpaEnabled = settings_->dangerTcpaEnabled();
        r.tcpaMin     = settings_->dangerTcpaMin();
        r.anchoredSafeEnabled = settings_->dangerAnchoredSafeEnabled();
        r.anchoredSogKn       = settings_->dangerAnchoredSogKn();
        aisOverlay_->setDangerRules(r);
        if (aisAlarm_) {
            aisAlarm_->setRules(r);
            aisAlarm_->setSoundEnabled(settings_->dangerAlarmSound());
        }
        if (view_) view_->requestRepaint(RepaintReason::Immediate);
    };
    applyDangerRules();
    connect(settings_, &Settings::dangerousShipsChanged, this, applyDangerRules);
    connect(settings_, &Settings::aisStaleThresholdsChanged,
            this, [this](double staleS, double lostS) {
        if (aisStore_) {
            aisStore_->setStaleSeconds(staleS);
            aisStore_->setLostSeconds(lostS);
        }
    });
    connect(aisStore_, &AisTargetStore::targetUpdated, this, [this](quint32) {
        if (view_) view_->requestRepaint();
    });
    connect(aisStore_, &AisTargetStore::targetExpired, this, [this](quint32) {
        if (view_) view_->requestRepaint();
    });

    // Routes & waypoints: SQLite-backed store + chart overlay/editor. The overlay
    // is added after the AIS overlay so that, during an edit session, its
    // hitTest gets first refusal on a tap (reverse z-order) and can add/select
    // route nodes even over an AIS target. Store changes repaint the chart.
    routeStore_ = new RouteStore(this);
    routeOverlay_ = std::make_unique<RouteOverlay>(routeStore_);
    routeOverlay_->setNavSource(navStore_);   // for the active-waypoint highlight
    routeOverlay_->setUnitsSource(settings_);  // leg distance/heading label units
    routeOverlay_->setRepaintCallback(
        [this] { if (view_) view_->requestRepaint(RepaintReason::Immediate); });
    routeOverlay_->setWaypointPlacedCallback([this](double lat, double lon) {
        onWaypointPlaced(lat, lon);
    });
    routeOverlay_->setSelectionChangedCallback([this](bool has) {
        if (deletePointBtn_) deletePointBtn_->setEnabled(has);
    });
    routeOverlay_->setObjectClickedCallback([this](const ClickedRouteObject& hit) {
        onRouteObjectClicked(hit);
    });
    view_->addOverlay(routeOverlay_.get());
    connect(routeStore_, &RouteStore::routesChanged,    this, [this] { if (view_) view_->requestRepaint(); });
    connect(routeStore_, &RouteStore::waypointsChanged, this, [this] { if (view_) view_->requestRepaint(); });
    // Repaint when the active waypoint changes (advance / start / stop) so the
    // route overlay's red highlight tracks it promptly.
    connect(navStore_, &NavDataStore::navigationChanged, this, [this] { if (view_) view_->requestRepaint(); });

    // Collision component: computes each target's CPA/TCPA against the ownship
    // and writes them back into the store (which the overlay and info windows
    // read). Skips the boat's own MMSI so its AIS echo never alarms on itself.
    cpaCalc_ = new CpaCalculator(navStore_, aisStore_, this);
    cpaCalc_->setOwnshipMmsi(settings_->ownshipMmsi().toUInt());
    connect(settings_, &Settings::ownshipMmsiChanged, this, [this](const QString& m) {
        if (cpaCalc_) cpaCalc_->setOwnshipMmsi(m.toUInt());
    });

    // Touch-first navigation: a floating menu button over the chart opens the
    // side drawer. No toolbar, no right-click, large tap targets.
    sideMenu_ = new SideMenu(settings_, view_);
    // Auto-hide behaviour: drives both tap-outside dismiss and action auto-close.
    sideMenu_->setAutoHide(settings_->autoHideMenu());
    connect(settings_, &Settings::autoHideMenuChanged,
            sideMenu_, &SideMenu::setAutoHide);
    connect(sideMenu_, &SideMenu::centerOnOwnshipRequested, view_, &ChartView::centerOnOwnship);
    connect(sideMenu_, &SideMenu::zoomToChartsRequested,    view_, &ChartView::zoomToCharts);
    connect(sideMenu_, &SideMenu::autoFollowToggled,        view_, &ChartView::setAutoFollow);
    connect(view_, &ChartView::autoFollowChanged,           sideMenu_, &SideMenu::setAutoFollowChecked);
    connect(sideMenu_, &SideMenu::courseUpToggled,          view_, &ChartView::setCourseUp);
    connect(view_, &ChartView::courseUpChanged,             sideMenu_, &SideMenu::setCourseUpChecked);
    connect(sideMenu_, &SideMenu::chartSetToggled,          this,  &MainWindow::onChartSetToggled);
    connect(sideMenu_, &SideMenu::manageChartSetsRequested, this,  &MainWindow::manageChartSets);
    connect(sideMenu_, &SideMenu::prepareChartCacheRequested, this, &MainWindow::prepareChartCache);
    connect(sideMenu_, &SideMenu::basemapFolderRequested,   this,  &MainWindow::chooseBasemapFolder);
    connect(sideMenu_, &SideMenu::editUnitsRequested,       this,  &MainWindow::editUnits);
    connect(sideMenu_, &SideMenu::navigationOptionsRequested, this, &MainWindow::editNavigationOptions);
    connect(sideMenu_, &SideMenu::editStaleThresholdsRequested,     this, &MainWindow::editStaleThresholds);
    connect(sideMenu_, &SideMenu::editOwnshipPredictionRequested,   this, &MainWindow::editOwnshipPrediction);
    connect(sideMenu_, &SideMenu::navDataBrowserRequested,          this, &MainWindow::showNavDataBrowser);
    connect(sideMenu_, &SideMenu::editDataPriorityRequested,        this, &MainWindow::editDataPriority);
    connect(sideMenu_, &SideMenu::editChartDetailLevelRequested,    this, &MainWindow::editChartDetailLevel);
    connect(sideMenu_, &SideMenu::editSymbolSizeRequested,          this, &MainWindow::editSymbolSize);
    connect(sideMenu_, &SideMenu::editVesselSizeRequested,          this, &MainWindow::editVesselSize);
    connect(sideMenu_, &SideMenu::editOwnshipMmsiRequested,         this, &MainWindow::editOwnshipMmsi);
    connect(sideMenu_, &SideMenu::editHeadingSourceRequested,       this, &MainWindow::editHeadingSource);
    connect(sideMenu_, &SideMenu::editDangerousShipsRequested,      this, &MainWindow::editDangerousShips);
    connect(sideMenu_, &SideMenu::aisTargetListRequested,           this, &MainWindow::showAisTargetList);
    connect(sideMenu_, &SideMenu::aboutRequested,                   this, &MainWindow::showAbout);
    connect(sideMenu_, &SideMenu::createRouteRequested,    this, &MainWindow::startCreateRoute);
    connect(sideMenu_, &SideMenu::editRouteRequested,      this, &MainWindow::startEditRoute);
    connect(sideMenu_, &SideMenu::createWaypointRequested, this, &MainWindow::startCreateWaypoint);
    connect(sideMenu_, &SideMenu::editWaypointRequested,   this, &MainWindow::startEditWaypoint);
    connect(sideMenu_, &SideMenu::dropWaypointRequested,   this, &MainWindow::dropWaypoint);
    connect(sideMenu_, &SideMenu::routeWaypointListRequested, this, &MainWindow::showRouteWaypointList);

    // Route navigation engine. Computes APB/RMB values into the nav store while
    // active. Its active state and the menu's "Navigating" checkbox mirror each
    // other (setNavigatingChecked guards against the toggle feedback loop).
    navigator_ = new RouteNavigator(navStore_, routeStore_, settings_, this);
    connect(navigator_, &RouteNavigator::activeChanged,
            sideMenu_, &SideMenu::setNavigatingChecked);
    connect(navigator_, &RouteNavigator::navigationCompleted,
            this, &MainWindow::showNavigationCompleteBanner);
    connect(sideMenu_, &SideMenu::navigatingToggled,
            this, &MainWindow::onNavigatingToggled);
    // Floating readout over the chart; shows/hides itself with navigation state.
    navDisplay_ = new NavDisplayWindow(navStore_, view_);

    buildEditBar();   // floating Complete/Delete/Cancel bar (hidden until editing)

    // Plugin layer: the core exposes services through CoreApi; the manager owns
    // the built-in plugins and drives their lifecycle. Same interfaces a dynamic
    // plugin would use later. NMEA 0183/2000 are built-in plugins; dynamic
    // plugins are discovered alongside the exe. Plugins register their sources here.
    coreApi_ = std::make_unique<CoreApi>(navStore_, aisStore_, routeStore_, sideMenu_, view_,
                                         &registry_, &chartSources_, this);
    plugins_ = std::make_unique<PluginManager>(coreApi_.get());
    plugins_->add(std::make_unique<Nmea0183Plugin>());   // first => default-highest priority
    plugins_->add(std::make_unique<Nmea2000Plugin>());
    // Dynamic plugins (GPX, Signal K, WMM, Instruments, ...) discovered as shared
    // libraries in the plugin folder (plugins/ next to the exe, or Contents/PlugIns
    // on macOS) and loaded via QPluginLoader.
    plugins_->loadFromDirectory(bundlepaths::pluginDir());
    plugins_->initializeAll();

    // Apply the saved source-priority order across the registered sources
    // (NMEA 0183/2000 and any plugin sources).
    navStore_->setSourcePriority(registry_.orderedIds(settings_->dataSourcePriority()));

    menuButton_ = new QPushButton(QStringLiteral("☰"), view_);  // hamburger
    menuButton_->setFixedSize(48, 48);
    menuButton_->setCursor(Qt::PointingHandCursor);
    // Pin colour explicitly: the hamburger glyph otherwise inherits the system
    // text colour (white in dark mode) which becomes invisible on a translucent
    // light background. The overlayBtn palette swaps for the OS theme.
    const theme::OverlayBtnPalette& ob = theme::overlayBtn();
    menuButton_->setStyleSheet(QStringLiteral(
        "QPushButton{ font-size:22px; color:%1; border:1px solid %2;"
        " border-radius:24px; background:%3; }"
        "QPushButton:pressed{ background:%4; }")
        .arg(ob.fg, ob.border, ob.bg, ob.pressed));
    connect(menuButton_, &QPushButton::clicked, sideMenu_, &SideMenu::openMenu);
    menuButton_->show();

    // Floating "+" button: a visible affordance for creating routes/waypoints
    // without diving into the menu. Same look as the menu button, sits just
    // below it. Long-press on the chart opens the same popup at the tap site.
    addButton_ = new QPushButton(QStringLiteral("+"), view_);
    addButton_->setFixedSize(48, 48);
    addButton_->setCursor(Qt::PointingHandCursor);
    addButton_->setStyleSheet(QStringLiteral(
        "QPushButton{ font-size:26px; font-weight:600; color:%1;"
        " border:1px solid %2; border-radius:24px; background:%3;"
        " padding-bottom:4px; }"
        "QPushButton:pressed{ background:%4; }")
        .arg(ob.fg, ob.border, ob.bg, ob.pressed));
    addButton_->setToolTip(QStringLiteral("Add a route or waypoint"));
    connect(addButton_, &QPushButton::clicked, this, [this] {
        // The "+" button has no chart-position context, so the next chart tap
        // places the first point (screenPt is unused in this path).
        showAddPopup(QPointF(), addButton_->mapToGlobal(QPoint(addButton_->width(), 0)), /*atPoint=*/false);
    });
    addButton_->show();

    // Floating "Layers" button: opens a small panel to toggle chart display
    // layers (Soundings, Symbols, Text, Depth Contours). Same round look, sits
    // just below the "+" button. The glyph is a painted stacked-sheets icon.
    layersButton_ = new QPushButton(view_);
    layersButton_->setFixedSize(48, 48);
    layersButton_->setCursor(Qt::PointingHandCursor);
    layersButton_->setIcon(QIcon(makeLayersIcon(QColor(ob.fg), 26)));
    layersButton_->setIconSize(QSize(26, 26));
    layersButton_->setStyleSheet(QStringLiteral(
        "QPushButton{ border:1px solid %1; border-radius:24px; background:%2; }"
        "QPushButton:pressed{ background:%3; }")
        .arg(ob.border, ob.bg, ob.pressed));
    layersButton_->setToolTip(QStringLiteral("Chart layers"));
    connect(layersButton_, &QPushButton::clicked, this, &MainWindow::showLayersDialog);
    layersButton_->show();

    // Floating "Course Up" toggle: a compass button under the Layers button that
    // turns course-up chart rotation on/off. Checkable, always visible; its state
    // mirrors the view (a pan that drops auto-follow also drops course-up, which
    // the view reports back via courseUpChanged -> syncCourseUpButton).
    courseUpButton_ = new QPushButton(view_);
    courseUpButton_->setFixedSize(48, 48);
    courseUpButton_->setCheckable(true);
    courseUpButton_->setCursor(Qt::PointingHandCursor);
    courseUpButton_->setFocusPolicy(Qt::NoFocus);
    courseUpButton_->setIconSize(QSize(26, 26));
    courseUpButton_->setStyleSheet(QStringLiteral(
        "QPushButton{ border:1px solid %1; border-radius:24px; background:%2; }"
        "QPushButton:checked{ background:#2f6bad; border-color:#2f6bad; }"
        "QPushButton:pressed{ background:%3; }")
        .arg(ob.border, ob.bg, ob.pressed));
    courseUpButton_->setToolTip(QStringLiteral("Course up"));
    connect(courseUpButton_, &QPushButton::clicked, this,
            [this](bool on) { view_->setCourseUp(on); });
    connect(view_, &ChartView::courseUpChanged, this, &MainWindow::syncCourseUpButton);
    connect(view_, &ChartView::viewRotationChanged, this, &MainWindow::setCompassHeading);
    syncCourseUpButton(false);   // initial icon (unchecked, north-up)
    courseUpButton_->show();

    // Long-press on the chart opens the same popup at the tap.
    connect(view_, &ChartView::longPressed, this, &MainWindow::onChartLongPressed);

    view_->installEventFilter(this);   // reposition the buttons when the view resizes
    positionMenuButton();
    positionAddButton();
    positionLayersButton();
    positionCourseUpButton();

    statusLeft_  = new QLabel(QStringLiteral("No chart folder selected"));
    statusMid_   = new QLabel(QString());
    statusRight_ = new QLabel(QString());
    statusBar()->addWidget(statusLeft_, 1);
    statusBar()->addPermanentWidget(statusMid_);
    statusBar()->addPermanentWidget(statusRight_);

    const QStringList selected = settings_->selectedDirectories();
    bool anyExist = false;
    for (const QString& d : selected)
        if (QDir(d).exists()) { anyExist = true; break; }
    if (anyExist) {
        if (settings_->hasSavedView())
            view_->setInitialView(settings_->viewLon(), settings_->viewLat(),
                                  settings_->viewScale());
        rescanSelected();
    }
}

// Defined here (not =default in the header) so CoreApi/PluginManager are complete
// types when the unique_ptr members are destroyed. The manager shuts plugins down
// (removing overlays from the still-alive ChartView) before they are freed.
MainWindow::~MainWindow() {
    hmvtrace::mark("~MainWindow begin (plugins shut down, then children destroyed)");
}

bool MainWindow::eventFilter(QObject* obj, QEvent* e) {
    if (obj == view_ && e->type() == QEvent::Resize) {
        positionMenuButton();
        positionAddButton();
        positionLayersButton();
        positionCourseUpButton();
        positionEditBar();
        positionNavBanner();
    }
    return QMainWindow::eventFilter(obj, e);
}

void MainWindow::closeEvent(QCloseEvent* e) {
    hmvtrace::mark("MainWindow::closeEvent begin");
    view_->persistViewNow();   // flush the latest location even if mid-debounce
    // Persist size / position / maximised state for the next launch.
    QSettings().setValue(QStringLiteral("window/geometry"), saveGeometry());
    // Cancel and join the catalog scan here, on the GUI thread while everything
    // is still alive, so it is never left running on the global pool (which would
    // otherwise be joined only at static teardown — after the window is gone —
    // and keep the process alive). Cooperative cancel makes this prompt.
    if (catalog_) { hmvtrace::mark("catalog shutdown begin"); catalog_->shutdown(); }
    hmvtrace::mark("catalog shutdown done");
    QMainWindow::closeEvent(e);
    hmvtrace::mark("MainWindow::closeEvent end");
}

void MainWindow::positionMenuButton() {
    if (!menuButton_) return;
    menuButton_->move(12, 12);
    if (!sideMenu_ || !sideMenu_->isOpen())
        menuButton_->raise();   // stay above the chart, but never above an open menu
}

void MainWindow::positionAddButton() {
    if (!addButton_ || !menuButton_) return;
    // Stack directly below the menu button, same left edge, small gap.
    addButton_->move(12, 12 + menuButton_->height() + 8);
    if (!sideMenu_ || !sideMenu_->isOpen()) addButton_->raise();
}

void MainWindow::positionLayersButton() {
    if (!layersButton_ || !addButton_) return;
    // Stack directly below the "+" button, same left edge, matching gap.
    layersButton_->move(12, addButton_->y() + addButton_->height() + 8);
    if (!sideMenu_ || !sideMenu_->isOpen()) layersButton_->raise();
}

void MainWindow::positionCourseUpButton() {
    if (!courseUpButton_ || !layersButton_) return;
    // Stack directly below the Layers button, same left edge, matching gap.
    courseUpButton_->move(12, layersButton_->y() + layersButton_->height() + 8);
    if (!sideMenu_ || !sideMenu_->isOpen()) courseUpButton_->raise();
}

void MainWindow::syncCourseUpButton(bool on) {
    if (!courseUpButton_) return;
    if (courseUpButton_->isChecked() != on) courseUpButton_->setChecked(on);
    if (!on) courseUpAngle_ = 0.0;   // north-up when course-up is off
    refreshCompassIcon();
}

void MainWindow::setCompassHeading(double upDegrees) {
    courseUpAngle_ = upDegrees;
    refreshCompassIcon();
}

void MainWindow::refreshCompassIcon() {
    if (!courseUpButton_) return;
    // White needle on the blue "on" fill; themed foreground otherwise. The needle
    // points to true north = screen-up rotated back by the view's up-bearing.
    const QColor col = courseUpButton_->isChecked() ? QColor(255, 255, 255)
                                                    : QColor(theme::overlayBtn().fg);
    courseUpButton_->setIcon(QIcon(makeCompassIcon(col, 26, -courseUpAngle_)));
}

void MainWindow::showLayersDialog() {
    // One panel at a time; reuse it if it's already open (QPointer clears itself
    // when the WA_DeleteOnClose window is dismissed, so this recreates a fresh
    // one after a close).
    if (!layersDlg_) {
        layersDlg_ = new LayersDialog(settings_, this);
        layersDlg_->setAttribute(Qt::WA_DeleteOnClose);
    }
    layersDlg_->adjustSize();
    // Anchor to the right of the layers button, clamped to the screen.
    QPoint anchor = layersButton_->mapToGlobal(QPoint(layersButton_->width() + 10, 0));
    const QRect screen = view_->screen() ? view_->screen()->availableGeometry()
                                         : QRect(0, 0, 1920, 1080);
    anchor.setX(std::min(anchor.x(), screen.right()  - layersDlg_->width()  - 12));
    anchor.setY(std::min(anchor.y(), screen.bottom() - layersDlg_->height() - 12));
    layersDlg_->move(anchor);
    layersDlg_->show();
    layersDlg_->raise();
    layersDlg_->activateWindow();
}

void MainWindow::onChartSetToggled(const QString& dir) {
    // Tapping a set adds it to (or removes it from) the active selection; all
    // selected sets are then (re)loaded together. Keep the current pan/zoom
    // across the change rather than refitting to the new combined coverage.
    QStringList sel = settings_->selectedDirectories();
    if (sel.contains(dir)) sel.removeAll(dir);
    else                   sel.append(dir);
    view_->keepCurrentViewOnNextLoad();
    settings_->setSelectedDirectories(sel);
    rescanSelected();
}

void MainWindow::manageChartSets() {
    const bool hadSelection = !settings_->selectedDirectories().isEmpty();
    ChartSetsDialog dlg(settings_->chartSets(), this);
    if (dlg.exec() == QDialog::Accepted) {
        settings_->setChartSets(dlg.chartSets());
        // If nothing was selected before and the user just added the first set,
        // activate it automatically so they don't close the menu to an empty
        // chart with no indication of what to do next.
        if (!hadSelection && !dlg.chartSets().isEmpty())
            onChartSetToggled(dlg.chartSets().first().directory);
    }
}

void MainWindow::prepareChartCache() {
    if (catalog_->isScanning()) {
        QMessageBox::information(this, QStringLiteral("Prepare Chart Cache"),
            QStringLiteral("A chart scan is still running. Try again once "
                           "cataloging has finished."));
        return;
    }

    // Only the built-in ENC reader produces real file-path cell ids that the
    // parsed-cell cache can key on. If a plugin backend (e.g. CM93) is active
    // for the selection, there is nothing to prepare here yet.
    IChartSource* activeSrc = nullptr;
    for (const QString& d : settings_->selectedDirectories()) {
        if (IChartSource* s = chartSources_.pick(d)) { activeSrc = s; break; }
    }
    if (activeSrc) {
        QMessageBox::information(this, QStringLiteral("Prepare Chart Cache"),
            QStringLiteral("The active chart set uses the \"%1\" backend, which "
                           "is not cached yet. Only built-in ENC charts can be "
                           "prepared.").arg(activeSrc->displayName()));
        return;
    }

    QStringList paths;
    paths.reserve(static_cast<int>(catalog_->cells().size()));
    for (const CellRecord& c : catalog_->cells())
        paths << c.path;

    if (paths.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Prepare Chart Cache"),
            QStringLiteral("No ENC chart cells are catalogued. Select a chart "
                           "set with ENC charts first."));
        return;
    }

    PrepareCacheDialog dlg(paths, this);
    dlg.exec();
}

void MainWindow::chooseBasemapFolder() {
    const QString start = settings_->basemapDirectory();
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select GSHHG Basemap Folder (contains GSHHS_shp)"), start);
    if (!dir.isEmpty())
        settings_->setBasemapDirectory(dir);
}

void MainWindow::editUnits() {
    UnitsDialog dlg(settings_->depthUnit(), settings_->distanceUnit(),
                    settings_->angleFormat(), settings_->bearingMode(), this);
    if (dlg.exec() == QDialog::Accepted) {
        settings_->setDepthUnit(dlg.depthUnit());
        settings_->setDistanceUnit(dlg.distanceUnit());
        settings_->setAngleFormat(dlg.angleFormat());
        settings_->setBearingMode(dlg.bearingMode());
    }
}

void MainWindow::editNavigationOptions() {
    NavigationOptionsDialog dlg(settings_->arrivalRadiusNm(), this);
    if (dlg.exec() == QDialog::Accepted)
        settings_->setArrivalRadiusNm(dlg.arrivalRadiusNm());
}

void MainWindow::editStaleThresholds() {
    StaleThresholdsDialog dlg(settings_->staleSeconds(), settings_->invalidSeconds(),
                              settings_->aisStaleSeconds(), settings_->aisLostSeconds(),
                              this);
    if (dlg.exec() == QDialog::Accepted) {
        settings_->setStaleThresholds(dlg.staleSeconds(), dlg.invalidSeconds());
        settings_->setAisStaleThresholds(dlg.aisStaleSeconds(), dlg.aisLostSeconds());
    }
}

void MainWindow::editOwnshipPrediction() {
    OwnshipPredictionDialog dlg(settings_->ownshipPredictionMinutes(), this);
    if (dlg.exec() == QDialog::Accepted)
        settings_->setOwnshipPredictionMinutes(dlg.minutes());
}

void MainWindow::showNavDataBrowser() {
    if (!navBrowser_)
        navBrowser_ = new NavDataBrowserWindow(navStore_, this);
    navBrowser_->show();
    navBrowser_->raise();
    navBrowser_->activateWindow();
}

void MainWindow::editDataPriority() {
    // Show all registered sources (built-in + plugin) in the saved order.
    DataPriorityDialog dlg(registry_.ordered(settings_->dataSourcePriority()), this);
    if (dlg.exec() == QDialog::Accepted)
        settings_->setDataSourcePriority(dlg.orderedIds());
}

void MainWindow::editChartDetailLevel() {
    ChartDetailDialog dlg(settings_->chartDetailLevel(),
                          settings_->chartScaminLevel(), this);
    if (dlg.exec() == QDialog::Accepted) {
        settings_->setChartDetailLevel(dlg.detailLevel());
        settings_->setChartScaminLevel(dlg.scaminLevel());
    }
}

void MainWindow::editSymbolSize() {
    ChartSymbolSizeDialog dlg(settings_->symbolScale(), this);
    if (dlg.exec() == QDialog::Accepted)
        settings_->setSymbolScale(dlg.symbolScale());
}

void MainWindow::editVesselSize() {
    ShipSizeDialog dlg(settings_->vesselScale(), this);
    if (dlg.exec() == QDialog::Accepted)
        settings_->setVesselScale(dlg.vesselScale());
}

void MainWindow::editOwnshipMmsi() {
    OwnshipMmsiDialog dlg(settings_->ownshipMmsi(), this);
    if (dlg.exec() == QDialog::Accepted)
        settings_->setOwnshipMmsi(dlg.mmsi());
}

void MainWindow::editHeadingSource() {
    HeadingSourceDialog dlg(settings_->headingSource(), this);
    if (dlg.exec() == QDialog::Accepted)
        settings_->setHeadingSource(dlg.source());
}

void MainWindow::editDangerousShips() {
    DangerousShipsDialog dlg(settings_->dangerIgnoreFarEnabled(), settings_->dangerIgnoreFarNm(),
                             settings_->dangerCpaEnabled(), settings_->dangerCpaNm(),
                             settings_->dangerTcpaEnabled(), settings_->dangerTcpaMin(),
                             settings_->dangerAnchoredSafeEnabled(), settings_->dangerAnchoredSogKn(),
                             settings_->dangerAlarmSound(),
                             this);
    if (dlg.exec() == QDialog::Accepted)
        settings_->setDangerousShips(dlg.ignoreFarEnabled(), dlg.ignoreFarNm(),
                                     dlg.cpaEnabled(), dlg.cpaNm(),
                                     dlg.tcpaEnabled(), dlg.tcpaMin(),
                                     dlg.anchoredSafeEnabled(), dlg.anchoredSogKn(),
                                     dlg.alarmSoundEnabled());
}

// ---- Routes & Waypoints ----------------------------------------------------

void MainWindow::buildEditBar() {
    // A compact floating toolbar over the chart, used while creating/editing a
    // route or placing a waypoint. Child of the view so it overlays the chart.
    editBar_ = new QWidget(view_);
    editBar_->setStyleSheet(QStringLiteral(
        "QWidget{ background: rgba(30,34,40,235); border:1px solid rgba(255,255,255,40);"
        " border-radius:8px; }"
        "QLabel{ color:#e6e9ee; font-size:13px; background:transparent; border:none; }"
        "QPushButton{ font-size:14px; min-height:38px; padding:0 14px;"
        " color:#ffffff; border:1px solid rgba(255,255,255,60); border-radius:6px;"
        " background: rgba(255,255,255,16); }"
        "QPushButton:disabled{ color: rgba(255,255,255,90); }"
        "QPushButton:pressed{ background: rgba(255,255,255,40); }"));
    auto* row = new QHBoxLayout(editBar_);
    row->setContentsMargins(12, 8, 12, 8);
    row->setSpacing(8);

    editHint_ = new QLabel(editBar_);
    row->addWidget(editHint_);
    row->addSpacing(4);

    deletePointBtn_ = new QPushButton(QStringLiteral("Delete Point"), editBar_);
    deletePointBtn_->setEnabled(false);
    connect(deletePointBtn_, &QPushButton::clicked, this, [this] {
        if (routeOverlay_) routeOverlay_->deleteSelectedNode();
    });
    row->addWidget(deletePointBtn_);

    completeBtn_ = new QPushButton(QStringLiteral("Complete Route"), editBar_);
    connect(completeBtn_, &QPushButton::clicked, this, &MainWindow::completeEdit);
    row->addWidget(completeBtn_);

    cancelEditBtn_ = new QPushButton(QStringLiteral("Cancel"), editBar_);
    connect(cancelEditBtn_, &QPushButton::clicked, this, &MainWindow::cancelEdit);
    row->addWidget(cancelEditBtn_);

    editBar_->hide();
}

void MainWindow::positionEditBar() {
    if (!editBar_ || !editBar_->isVisible() || !view_) return;
    editBar_->adjustSize();
    const int x = (view_->width() - editBar_->width()) / 2;
    editBar_->move(std::max(8, x), 12);
    editBar_->raise();
}

void MainWindow::showNavigationCompleteBanner() {
    if (!view_) return;
    // Lazily build the banner, reusing the edit bar's dark style. It's a child of
    // the view so it overlays the chart top, and self-dismisses after a few
    // seconds (or when the user starts navigating again).
    if (!navBanner_) {
        navBanner_ = new QLabel(view_);
        navBanner_->setAlignment(Qt::AlignCenter);
        navBanner_->setStyleSheet(QStringLiteral(
            "QLabel{ background: rgba(30,34,40,235); color:#e6e9ee;"
            " font-size:15px; font-weight:600; padding:10px 18px;"
            " border:1px solid rgba(255,255,255,40); border-radius:8px; }"));
        navBanner_->hide();
    }
    navBanner_->setText(QStringLiteral("Navigation Complete."));
    navBanner_->show();
    positionNavBanner();
    QTimer::singleShot(4000, navBanner_, [this] {
        if (navBanner_) navBanner_->hide();
    });
}

void MainWindow::positionNavBanner() {
    if (!navBanner_ || !navBanner_->isVisible() || !view_) return;
    navBanner_->adjustSize();
    const int x = (view_->width() - navBanner_->width()) / 2;
    navBanner_->move(std::max(8, x), 12);
    navBanner_->raise();
}

void MainWindow::showRouteEditBar(const QString& hint) {
    editHint_->setText(hint);
    completeBtn_->setText(QStringLiteral("Complete Route"));
    deletePointBtn_->show();
    completeBtn_->show();
    deletePointBtn_->setEnabled(false);
    editBar_->show();
    positionEditBar();
}

void MainWindow::showWaypointPlaceBar(const QString& hint) {
    editHint_->setText(hint);
    deletePointBtn_->hide();
    completeBtn_->hide();
    editBar_->show();
    positionEditBar();
}

void MainWindow::showWaypointEditBar(const QString& hint) {
    editHint_->setText(hint);
    completeBtn_->setText(QStringLiteral("Done"));
    deletePointBtn_->hide();
    completeBtn_->show();
    editBar_->show();
    positionEditBar();
}

void MainWindow::showPointDragBar(const QString& hint) {
    // Done/Cancel only — deletion of route points is handled in the Properties
    // dialog, so no Delete Point button here.
    editHint_->setText(hint);
    completeBtn_->setText(QStringLiteral("Done"));
    deletePointBtn_->hide();
    completeBtn_->show();
    editBar_->show();
    positionEditBar();
}

void MainWindow::endRouteMode() {
    if (routeOverlay_) routeOverlay_->endEditing();
    if (view_) view_->setChartEditor(nullptr);
    if (editBar_) editBar_->hide();
    if (view_) view_->requestRepaint(RepaintReason::Immediate);
}

void MainWindow::startCreateRoute() {
    if (sideMenu_) sideMenu_->closeMenu();
    routeOverlay_->beginCreateRoute();
    view_->setChartEditor(routeOverlay_.get());
    showRouteEditBar(QStringLiteral("Tap the chart to add points · drag to move · tap a point to select"));
}

void MainWindow::onRouteObjectClicked(const ClickedRouteObject& hit) {
    if (!routeStore_) return;
    // Replace any existing popup; close any AIS popup that was up.
    if (aisQuickInfo_) aisQuickInfo_->close();
    if (routeQuickInfo_) routeQuickInfo_->close();

    routeQuickInfo_ = new RouteQuickInfoWindow(hit.kind, hit.id, routeStore_, this);
    const qint64 id = hit.id;
    if (hit.kind == ClickedRouteObject::Kind::Waypoint) {
        connect(routeQuickInfo_, &RouteQuickInfoWindow::renameRequested,
                this, [this, id] { renameWaypoint(id); });
        connect(routeQuickInfo_, &RouteQuickInfoWindow::editRequested,
                this, [this, id] { beginEditWaypoint(id); });
        connect(routeQuickInfo_, &RouteQuickInfoWindow::propertiesRequested,
                this, [this, id] { openWaypointProperties(id); });
        connect(routeQuickInfo_, &RouteQuickInfoWindow::visibilityToggleRequested,
                this, [this, id] { toggleWaypointVisible(id); });
        connect(routeQuickInfo_, &RouteQuickInfoWindow::deleteRequested,
                this, [this, id] { confirmDeleteWaypoint(id); });
    } else {
        const int startIdx = hit.pointIndex;   // the tapped waypoint / leg-end
        connect(routeQuickInfo_, &RouteQuickInfoWindow::navigateRequested,
                this, [this, id, startIdx] { startNavigation(id, startIdx); });
        connect(routeQuickInfo_, &RouteQuickInfoWindow::renameRequested,
                this, [this, id] { renameRoute(id); });
        connect(routeQuickInfo_, &RouteQuickInfoWindow::editRequested,
                this, [this, id] { beginEditRoute(id); });
        connect(routeQuickInfo_, &RouteQuickInfoWindow::propertiesRequested,
                this, [this, id] { openRouteProperties(id); });
        connect(routeQuickInfo_, &RouteQuickInfoWindow::visibilityToggleRequested,
                this, [this, id] { toggleRouteVisible(id); });
        connect(routeQuickInfo_, &RouteQuickInfoWindow::deleteRequested,
                this, [this, id] { confirmDeleteRoute(id); });
    }
    // Anchor near the tap, clamped inside the view so the popup is never
    // off-screen on a small window.
    routeQuickInfo_->adjustSize();
    QPoint anchor = view_->mapToGlobal(hit.screenPt.toPoint()) + QPoint(12, 12);
    const QRect screen = view_->screen()
        ? view_->screen()->availableGeometry()
        : QRect(0, 0, 1920, 1080);
    anchor.setX(std::min(anchor.x(), screen.right() - routeQuickInfo_->width() - 12));
    anchor.setY(std::min(anchor.y(), screen.bottom() - routeQuickInfo_->height() - 12));
    routeQuickInfo_->move(anchor);
    routeQuickInfo_->show();
}

void MainWindow::onObjectsPicked(const QList<ChartObjectInfo>& objects,
                                 const QPoint& globalPos) {
    if (objects.isEmpty()) return;
    if (objects.size() == 1) { showObjectInfo(objects.front(), globalPos); return; }

    // Several objects under the click: tap one to inspect it. Touch-friendly —
    // rows are large flat buttons and the list drag-scrolls via QScroller, like
    // the AIS target list; a single tap selects (no OK button).
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Chart objects here"));
    // Frameless + side-menu palette (light/dark aware) instead of the system
    // dialog chrome, so the chooser matches the rest of the app.
    dlg.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dlg.resize(320, 360);

    const theme::MenuPalette& th = theme::menu();

    auto* col = new QVBoxLayout(&dlg);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    // Bordered panel so the frameless window still has a visible edge.
    auto* panel = new QFrame(&dlg);
    panel->setObjectName(QStringLiteral("ObjectChooserPanel"));
    panel->setStyleSheet(QStringLiteral(
        "#ObjectChooserPanel{ background:%1; border:1px solid %2; }")
        .arg(th.panelBg, th.panelBorder));
    col->addWidget(panel);

    auto* panelCol = new QVBoxLayout(panel);
    panelCol->setContentsMargins(0, 0, 0, 0);
    panelCol->setSpacing(0);

    // Title bar, mirroring the side-menu header: a brand-navy strip with the
    // title and a close "✕" (the window is frameless, so this is the dismiss).
    // Dragging the bar moves the (frameless) window.
    auto* header = new QWidget(panel);
    header->setStyleSheet(QStringLiteral("background:%1;").arg(th.titleBg));
    header->setCursor(Qt::SizeAllCursor);
    header->installEventFilter(new WindowDragger(&dlg));
    auto* headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(16, 8, 8, 8);
    headerRow->setSpacing(6);
    auto* title = new QLabel(QStringLiteral("Chart objects here"), header);
    title->setAttribute(Qt::WA_TransparentForMouseEvents);   // clicks fall to the bar (drag)
    title->setStyleSheet(QStringLiteral(
        "font-size:18px; font-weight:600; background:transparent; color:%1;").arg(th.titleFg));
    headerRow->addWidget(title, 1);
    auto* closeBtn = new QPushButton(QString(QChar(0x2715)), header);   // ✕
    closeBtn->setFlat(true);
    closeBtn->setFixedSize(44, 44);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setStyleSheet(QStringLiteral(
        "QPushButton{ border:none; background:transparent; color:%1; font-size:18px; }"
        "QPushButton:pressed{ background:%2; border-radius:6px; }")
        .arg(th.titleFg, th.actionPressed));
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    headerRow->addWidget(closeBtn);
    panelCol->addWidget(header);

    auto* scroll = new QScrollArea(panel);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea, QScrollArea > QWidget > QWidget { background:%1; }").arg(th.panelBg));
    auto* rows = new QWidget;
    auto* rowCol = new QVBoxLayout(rows);
    rowCol->setContentsMargins(0, 0, 0, 0);
    rowCol->setSpacing(0);

    int chosen = -1;
    for (int i = 0; i < objects.size(); ++i) {
        const ChartObjectInfo& o = objects.at(i);
        const QString cls = chartObjectClassName(o.objClass);
        auto* btn = new QPushButton(o.name.isEmpty() ? cls
                                    : QStringLiteral("%1 — %2").arg(cls, o.name));
        btn->setFlat(true);
        btn->setMinimumHeight(56);
        btn->setCursor(Qt::PointingHandCursor);
        // Rows styled as side-menu actions: pinned background + text colour so
        // they render correctly in either theme, with a per-row separator.
        btn->setStyleSheet(QStringLiteral(
            "QPushButton{ text-align:left; border:none; padding:0 24px; font-size:16px;"
            " background:%1; color:%2; border-bottom:1px solid %3; }"
            "QPushButton:pressed{ background:%4; }")
            .arg(th.actionBg, th.actionFg, th.separator, th.actionPressed));
        connect(btn, &QPushButton::clicked, &dlg, [&dlg, &chosen, i] {
            chosen = i;
            dlg.accept();
        });
        rowCol->addWidget(btn);
    }
    rowCol->addStretch(1);
    scroll->setWidget(rows);
    QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);
    panelCol->addWidget(scroll, 1);

    // Anchor near the tap but clamp inside the screen so the whole window is
    // visible (e.g. a click near the bottom edge would otherwise be cut off).
    QPoint anchor = globalPos + QPoint(12, 12);
    const QRect screen = view_->screen() ? view_->screen()->availableGeometry()
                                         : QRect(0, 0, 1920, 1080);
    // Pull back from the right/bottom edges first, then guarantee the top-left
    // stays on-screen (min-then-max, so it's safe even if the window is large).
    anchor.setX(std::min(anchor.x(), screen.right()  - dlg.width()  - 12));
    anchor.setY(std::min(anchor.y(), screen.bottom() - dlg.height() - 12));
    anchor.setX(std::max(anchor.x(), screen.left() + 12));
    anchor.setY(std::max(anchor.y(), screen.top()  + 12));
    dlg.move(anchor);
    if (dlg.exec() == QDialog::Accepted && chosen >= 0)
        showObjectInfo(objects.at(chosen), globalPos);
}

void MainWindow::showAbout() {
    AboutDialog dlg(this);
    dlg.exec();
}

void MainWindow::showObjectInfo(const ChartObjectInfo& obj, const QPoint& globalPos) {
    // A chart-object click supersedes any open quick-look / object popup.
    if (aisQuickInfo_)   aisQuickInfo_->close();
    if (routeQuickInfo_) routeQuickInfo_->close();
    if (objectInfo_)     objectInfo_->close();

    objectInfo_ = new ChartObjectInfoWindow(obj, settings_->depthUnit(), this);
    objectInfo_->setAttribute(Qt::WA_DeleteOnClose);
    objectInfo_->adjustSize();
    QPoint anchor = globalPos + QPoint(12, 12);
    const QRect screen = view_->screen() ? view_->screen()->availableGeometry()
                                         : QRect(0, 0, 1920, 1080);
    anchor.setX(std::min(anchor.x(), screen.right()  - objectInfo_->width()  - 12));
    anchor.setY(std::min(anchor.y(), screen.bottom() - objectInfo_->height() - 12));
    objectInfo_->move(anchor);
    objectInfo_->show();
}

void MainWindow::renameRoute(qint64 id) {
    if (!routeStore_) return;
    const Route* r = routeStore_->route(id);
    if (!r) return;
    NameDialog dlg(QStringLiteral("Rename Route"), r->name, r->description, this);
    if (dlg.exec() != QDialog::Accepted) return;
    Route copy = *r;
    copy.name = dlg.name();
    copy.description = dlg.description();
    routeStore_->updateRoute(copy);
}

void MainWindow::renameWaypoint(qint64 id) {
    if (!routeStore_) return;
    const Waypoint* w = nullptr;
    for (const Waypoint& cand : routeStore_->waypoints())
        if (cand.id == id) { w = &cand; break; }
    if (!w) return;
    NameDialog dlg(QStringLiteral("Rename Waypoint"), w->name, w->description, this);
    if (dlg.exec() != QDialog::Accepted) return;
    Waypoint copy = *w;
    copy.name = dlg.name();
    copy.description = dlg.description();
    routeStore_->updateWaypoint(copy);
}

void MainWindow::toggleRouteVisible(qint64 id) {
    if (!routeStore_) return;
    const Route* r = routeStore_->route(id);
    if (r) routeStore_->setRouteVisible(id, !r->visible);
}

void MainWindow::toggleWaypointVisible(qint64 id) {
    if (!routeStore_) return;
    for (const Waypoint& w : routeStore_->waypoints())
        if (w.id == id) { routeStore_->setWaypointVisible(id, !w.visible); return; }
}

void MainWindow::confirmDeleteRoute(qint64 id) {
    if (!routeStore_) return;
    const Route* r = routeStore_->route(id);
    const QString nm = (r && !r->name.isEmpty()) ? r->name : QStringLiteral("this route");
    if (QMessageBox::question(this, QStringLiteral("Delete Route"),
            QStringLiteral("Delete %1?").arg(nm)) != QMessageBox::Yes) return;
    routeStore_->removeRoute(id);
}

void MainWindow::confirmDeleteWaypoint(qint64 id) {
    if (!routeStore_) return;
    QString nm = QStringLiteral("this waypoint");
    for (const Waypoint& w : routeStore_->waypoints())
        if (w.id == id) { if (!w.name.isEmpty()) nm = w.name; break; }
    if (QMessageBox::question(this, QStringLiteral("Delete Waypoint"),
            QStringLiteral("Delete %1?").arg(nm)) != QMessageBox::Yes) return;
    routeStore_->removeWaypoint(id);
}

void MainWindow::startNavigation(qint64 routeId, int destIndex) {
    if (navBanner_) navBanner_->hide();   // a fresh route clears the prior banner
    if (navigator_) navigator_->startRoute(routeId, destIndex);
}

void MainWindow::onNavigatingToggled(bool on) {
    if (!navigator_) return;
    if (on) {
        if (navigator_->isActive()) return;   // echo of a programmatic check; ignore
        // User ticked "Navigating" by hand: resume the last route if one remains,
        // otherwise there is nothing to navigate, so undo the tick.
        if (navigator_->canResume()) navigator_->resume();
        else if (sideMenu_)          sideMenu_->setNavigatingChecked(false);
    } else {
        navigator_->stop();
    }
}

void MainWindow::onChartLongPressed(const QPointF& screenPt) {
    // Long-press dismisses transient overlay popups (so a long-press on a saved
    // object doesn't leave its quick-info popup hanging) and then offers the add
    // menu at the press location.
    if (aisQuickInfo_)   aisQuickInfo_->close();
    if (routeQuickInfo_) routeQuickInfo_->close();
    showAddPopup(screenPt, view_->mapToGlobal(screenPt.toPoint()), /*atPoint=*/true);
}

void MainWindow::showAddPopup(const QPointF& screenPt, const QPoint& globalPt, bool atPoint) {
    QMenu menu(this);
    // The long-press knows where the user pressed, so it places "here"; the "+"
    // button has no position, so it arms creation and the next chart tap places.
    QAction* aWpt   = menu.addAction(atPoint ? QStringLiteral("New waypoint here")
                                             : QStringLiteral("New waypoint"));
    QAction* aRoute = menu.addAction(atPoint ? QStringLiteral("Start route here")
                                             : QStringLiteral("New route"));
    QAction* picked = menu.exec(globalPt);
    if (!picked || !routeStore_ || !routeOverlay_) return;

    if (picked == aWpt) {
        if (atPoint) {
            // Convert the press point to geo by running the overlay through the
            // chart-tap path: begin CreateWaypoint, then hitTest at screenPt,
            // which calls onWaypointPlaced -> auto-name save. Mode is reset in
            // onWaypointPlaced via endRouteMode().
            routeOverlay_->beginCreateWaypoint();
            routeOverlay_->hitTest(screenPt);
        } else {
            // Arm create-waypoint mode; the next chart tap places it.
            startCreateWaypoint();
        }
        return;
    }
    if (picked == aRoute) {
        // Start a fresh route session. For a long-press the first point is
        // placed at the press location; for the "+" button the route starts
        // empty and the first chart tap appends the first point.
        routeOverlay_->beginCreateRoute();
        view_->setChartEditor(routeOverlay_.get());
        if (atPoint) routeOverlay_->hitTest(screenPt);   // appends the first point
        showRouteEditBar(QStringLiteral("Tap the chart to add points · drag to move · tap a point to select"));
        return;
    }
}

void MainWindow::startEditRoute() {
    if (!routeStore_) return;
    // Modal picker on the Routes tab; a row tap accepts with the chosen id.
    RouteWaypointDialog dlg(routeStore_, RouteWaypointDialog::Tab::Routes, this);
    if (dlg.exec() == QDialog::Accepted && dlg.pickedId() >= 0) beginEditRoute(dlg.pickedId());
}

void MainWindow::beginEditRoute(qint64 id) {
    const Route* r = routeStore_->route(id);
    if (!r || r->points.isEmpty()) return;
    // Frame the route, then enter edit mode on a working copy.
    double latMin = 90, latMax = -90, lonMin = 180, lonMax = -180;
    for (const RoutePoint& p : r->points) {
        latMin = std::min(latMin, p.lat); latMax = std::max(latMax, p.lat);
        lonMin = std::min(lonMin, p.lon); lonMax = std::max(lonMax, p.lon);
    }
    view_->fitToGeoBox(latMin, lonMin, latMax, lonMax);
    if (sideMenu_) sideMenu_->closeMenu();
    routeOverlay_->beginEditRoute(*r);
    view_->setChartEditor(routeOverlay_.get());
    showRouteEditBar(QStringLiteral("Drag to move · tap empty chart to add · select a point to delete"));
}

void MainWindow::completeEdit() {
    // The single Complete/Done button serves route creation/editing, waypoint
    // moves, and the Route Properties point-drag round-trip.
    if (!routeOverlay_) return;
    if (editContext_ == EditContext::RouteProps) { finishPropsDrag(/*apply=*/true); return; }
    if (routeOverlay_->mode() == RouteOverlay::Mode::EditWaypoint)
        completeWaypointMove();
    else
        completeRoute();
}

void MainWindow::cancelEdit() {
    // Cancel discards the current edit. For a Properties point-drag it returns to
    // the dialog (without applying the drag); otherwise it ends the edit session.
    if (editContext_ == EditContext::RouteProps) { finishPropsDrag(/*apply=*/false); return; }
    endRouteMode();
}

void MainWindow::completeRoute() {
    if (!routeOverlay_ || !routeStore_) return;
    Route r = routeOverlay_->workingRoute();
    if (r.points.size() < 2) {
        QMessageBox::information(this, QStringLiteral("Route"),
            QStringLiteral("A route needs at least two points."));
        return;
    }
    // Auto-name new routes; reachable via Rename in the chart popup or via the
    // Properties dialog. Edits keep the existing name.
    if (r.id < 0 && r.name.isEmpty()) r.name = routeStore_->nextRouteName();
    if (r.id < 0) routeStore_->addRoute(r);
    else          routeStore_->updateRoute(r);
    endRouteMode();
}

void MainWindow::startCreateWaypoint() {
    if (sideMenu_) sideMenu_->closeMenu();
    routeOverlay_->beginCreateWaypoint();
    view_->setChartEditor(routeOverlay_.get());
    showWaypointPlaceBar(QStringLiteral("Tap the chart to place a waypoint"));
}

void MainWindow::onWaypointPlaced(double lat, double lon) {
    // Placing ends the create-waypoint mode and saves immediately with an
    // auto-name; rename from the chart popup or Properties.
    endRouteMode();
    Waypoint w;
    w.name = routeStore_->nextWaypointName();
    w.lat = lat; w.lon = lon;
    w.visible = true;
    routeStore_->addWaypoint(w);
}

void MainWindow::startEditWaypoint() {
    if (!routeStore_) return;
    RouteWaypointDialog dlg(routeStore_, RouteWaypointDialog::Tab::Waypoints, this);
    if (dlg.exec() == QDialog::Accepted && dlg.pickedId() >= 0) beginEditWaypoint(dlg.pickedId());
}

void MainWindow::beginEditWaypoint(qint64 id) {
    const Waypoint* found = nullptr;
    for (const Waypoint& w : routeStore_->waypoints())
        if (w.id == id) { found = &w; break; }
    if (!found) return;
    const Waypoint w = *found;   // copy before any repaint touches the store
    view_->fitToGeoBox(w.lat, w.lon, w.lat, w.lon);   // single point: padded by fitToGeoBox
    if (sideMenu_) sideMenu_->closeMenu();
    routeOverlay_->beginEditWaypoint(w);
    view_->setChartEditor(routeOverlay_.get());
    showWaypointEditBar(QStringLiteral("Drag the waypoint to move it, then Done"));
}

void MainWindow::completeWaypointMove() {
    if (!routeOverlay_ || !routeStore_) return;
    routeStore_->updateWaypoint(routeOverlay_->workingWaypoint());
    endRouteMode();
}

void MainWindow::dropWaypoint() {
    if (sideMenu_) sideMenu_->closeMenu();
    const OwnshipState& s = navStore_->ownship();
    if (!s.latitudeDeg.valid() || !s.longitudeDeg.valid()) {
        statusLeft_->setText(QStringLiteral("Drop Waypoint: no ownship position available"));
        return;
    }
    Waypoint w;
    w.name = routeStore_->nextWaypointName();
    w.lat = s.latitudeDeg.value;
    w.lon = s.longitudeDeg.value;
    w.visible = true;
    routeStore_->addWaypoint(w);
}

void MainWindow::showRouteWaypointList() {
    // One combined, modeless browser; its Routes/Waypoints tabs each relay their
    // own actions back here.
    if (!routeWptDlg_) {
        routeWptDlg_ = new RouteWaypointDialog(routeStore_, this);
        routeWptDlg_->setAttribute(Qt::WA_DeleteOnClose);
        connect(routeWptDlg_, &RouteWaypointDialog::routePropertiesRequested,
                this, &MainWindow::openRouteProperties);
        connect(routeWptDlg_, &RouteWaypointDialog::routeEditRequested,
                this, &MainWindow::beginEditRoute);
        connect(routeWptDlg_, &RouteWaypointDialog::newRouteRequested,
                this, &MainWindow::startCreateRoute);
        connect(routeWptDlg_, &RouteWaypointDialog::waypointPropertiesRequested,
                this, &MainWindow::openWaypointProperties);
        connect(routeWptDlg_, &RouteWaypointDialog::waypointEditRequested,
                this, &MainWindow::beginEditWaypoint);
        connect(routeWptDlg_, &RouteWaypointDialog::newWaypointAtOwnshipRequested,
                this, &MainWindow::dropWaypoint);
    }
    routeWptDlg_->show();
    routeWptDlg_->raise();
    routeWptDlg_->activateWindow();
}

void MainWindow::openRouteProperties(qint64 id) {
    if (!routeStore_) return;
    const Route* r = routeStore_->route(id);
    if (!r) return;
    // One Properties editor at a time; replace any existing.
    if (propsDlg_) propsDlg_->close();
    propsWork_ = *r;
    propsDlg_ = new RoutePropertiesDialog(*r, this);
    propsDlg_->setAttribute(Qt::WA_DeleteOnClose);
    connect(propsDlg_, &RoutePropertiesDialog::editPointRequested,
            this, &MainWindow::onPropsEditPoint);
    connect(propsDlg_, &QDialog::accepted, this, [this] {
        // Commit the edited working route to the store on OK.
        if (propsDlg_ && routeStore_) routeStore_->updateRoute(propsDlg_->currentRoute());
    });
    propsDlg_->show();
    propsDlg_->raise();
    propsDlg_->activateWindow();
}

void MainWindow::openWaypointProperties(qint64 id) {
    if (!routeStore_) return;
    const Waypoint* found = nullptr;
    for (const Waypoint& w : routeStore_->waypoints())
        if (w.id == id) { found = &w; break; }
    if (!found) return;
    // Modal: a waypoint has no on-chart drag handoff here (use Edit Waypoint for
    // that), so a simple OK/Cancel editor suffices.
    WaypointPropertiesDialog dlg(*found, this);
    if (dlg.exec() == QDialog::Accepted)
        routeStore_->updateWaypoint(dlg.currentWaypoint());
}

void MainWindow::onPropsEditPoint(int index) {
    if (!propsDlg_ || !routeOverlay_) return;
    // Capture the dialog's current edits (incl. any typed coords), then hand off
    // to the chart so the user can drag. The dialog hides during the drag.
    propsWork_ = propsDlg_->currentRoute();
    propsDlg_->hide();

    if (!propsWork_.points.isEmpty()) {
        double latMin = 90, latMax = -90, lonMin = 180, lonMax = -180;
        for (const RoutePoint& p : propsWork_.points) {
            latMin = std::min(latMin, p.lat); latMax = std::max(latMax, p.lat);
            lonMin = std::min(lonMin, p.lon); lonMax = std::max(lonMax, p.lon);
        }
        view_->fitToGeoBox(latMin, lonMin, latMax, lonMax);
    }
    routeOverlay_->beginEditRoute(propsWork_);
    view_->setChartEditor(routeOverlay_.get());
    editContext_ = EditContext::RouteProps;
    (void)index;   // the entry point; any node is then draggable
    showPointDragBar(QStringLiteral("Drag a point to move it, then Done"));
}

void MainWindow::finishPropsDrag(bool apply) {
    if (apply && routeOverlay_)
        propsWork_ = routeOverlay_->workingRoute();   // read back the dragged points
    editContext_ = EditContext::None;
    endRouteMode();                                   // clear overlay edit + bar
    if (propsDlg_) {
        propsDlg_->setRoute(propsWork_);              // reflect the new coordinates
        propsDlg_->show();
        propsDlg_->raise();
        propsDlg_->activateWindow();
    }
}

void MainWindow::showAisTargetList() {
    // Modeless: one instance, raised on subsequent opens; QPointer clears when
    // the dialog self-deletes so the next open creates a fresh one.
    if (!aisListDlg_) {
        aisListDlg_ = new AisTargetListDialog(aisStore_, this);
        aisListDlg_->setAttribute(Qt::WA_DeleteOnClose);
        // Tapping a row opens (or raises) the full info window for that MMSI,
        // skipping the chart's two-click quick-look since the list already
        // serves as the "first click".
        connect(aisListDlg_, &AisTargetListDialog::targetActivated,
                this, [this](quint32 mmsi) {
            AisTargetInfoWindow* w = aisInfoWindows_.value(mmsi);
            if (!w) {
                w = new AisTargetInfoWindow(mmsi, aisStore_, this);
                w->setAttribute(Qt::WA_DeleteOnClose);
                aisInfoWindows_.insert(mmsi, w);
            }
            w->show();
            w->raise();
            w->activateWindow();
        });
    }
    aisListDlg_->show();
    aisListDlg_->raise();
    aisListDlg_->activateWindow();
}

void MainWindow::showAisTarget(quint32 mmsi) {
    // Second click on the same target (its quick-look is still up) opens the
    // full info window.
    if (aisQuickInfo_ && aisQuickInfoMmsi_ == mmsi) {
        aisQuickInfo_->close();
        aisQuickInfoMmsi_ = 0;
        AisTargetInfoWindow* w = aisInfoWindows_.value(mmsi);
        if (!w) {
            w = new AisTargetInfoWindow(mmsi, aisStore_, this);
            w->setAttribute(Qt::WA_DeleteOnClose);
            aisInfoWindows_.insert(mmsi, w);
        }
        w->show();
        w->raise();
        w->activateWindow();
        return;
    }

    // First click (or a click on a different target): show the quick-look popup
    // near the cursor, replacing any popup already up for another target.
    if (aisQuickInfo_) aisQuickInfo_->close();
    auto* q = new AisQuickInfoWindow(mmsi, aisStore_, aisAlarm_, this);
    aisQuickInfo_     = q;
    aisQuickInfoMmsi_ = mmsi;
    q->move(QCursor::pos() + QPoint(14, 14));
    q->show();
}

void MainWindow::publishOwnshipToView() {
    if (view_) view_->setOwnship(navStore_->ownship());
}

void MainWindow::rescanSelected() {
    if (catalog_->isScanning()) return;
    const QStringList selected = settings_->selectedDirectories();

    encScanDone_ = false;
    encScanOk_ = false;
    encScanMsg_.clear();
    rasterCount_ = 0;

    // Pick the active vector backend: the source of the first selected set that
    // resolves to a plugin (e.g. CM93), else the built-in ENC/S-57 reader
    // (nullptr). The catalog and view share one backend, so vector sets that
    // need a *different* one can't be quilted together — those are skipped with a
    // note. (Raster is backend-independent and loads from every selected set.)
    IChartSource* activeSrc = nullptr;
    for (const QString& d : selected) {
        if (IChartSource* s = chartSources_.pick(d)) { activeSrc = s; break; }
    }
    QStringList vectorDirs;
    QStringList skipped;
    for (const QString& d : selected) {
        if (chartSources_.pick(d) == activeSrc) vectorDirs << d;
        else                                    skipped << QDir(d).dirName();
    }

    catalog_->setSource(activeSrc);
    view_->setChartSource(activeSrc);

    // Status prefix summarising the active selection.
    if (selected.isEmpty())        root_ = QStringLiteral("No chart sets selected");
    else if (selected.size() == 1) root_ = selected.first();
    else root_ = QStringLiteral("%1 chart sets").arg(selected.size());
    QString status = root_ + QStringLiteral("   —   scanning…");
    if (!skipped.isEmpty())
        status += QStringLiteral("   (different chart backend skipped: %1)")
                      .arg(skipped.join(QStringLiteral(", ")));
    statusLeft_->setText(status);
    statusMid_->clear();

    catalog_->startScan(vectorDirs);
    // The raster layer scans every selected folder for *.mbtiles, in parallel.
    view_->setRasterChartFolders(selected);
}

void MainWindow::onScanProgress(int done, int total) {
    statusLeft_->setText(root_ + QStringLiteral("   —   cataloging %1 / %2").arg(done).arg(total));
}

void MainWindow::onScanFinished(bool ok, const QString& message) {
    encScanDone_ = true;
    encScanOk_ = ok;
    encScanMsg_ = message;
    refreshChartStatus();
}

void MainWindow::onRasterChartsChanged(int count) {
    rasterCount_ = count;
    refreshChartStatus();
}

// Compose the status line from the two independent discovery results. A folder
// may hold ENC cells, raster (MBTiles) charts, both, or neither — so a folder
// with only raster charts is not treated as an error.
void MainWindow::refreshChartStatus() {
    QString s = root_ + QStringLiteral("   —   ");
    if (encScanOk_)              s += encScanMsg_;
    else if (rasterCount_ > 0)   s += QStringLiteral("no ENC cells");
    else if (encScanDone_)       s += QStringLiteral("no charts found");
    else                         s += QStringLiteral("scanning…");
    if (rasterCount_ > 0)
        s += QStringLiteral("   +   %1 raster chart(s)").arg(rasterCount_);
    statusLeft_->setText(s);
}

void MainWindow::onViewStatus(const QString& text) {
    statusMid_->setText(text);
}

void MainWindow::onCursorMoved(double lon, double lat) {
    lastCursorLon_ = lon;
    lastCursorLat_ = lat;
    statusRight_->setText(units::formatLatitude(lat) + QStringLiteral("   ")
                          + units::formatLongitude(lon));
}
