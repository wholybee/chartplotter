#include "side_menu.hpp"
#include "settings.hpp"
#include "theme.hpp"

#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QScrollArea>
#include <QScroller>
#include <QScrollerProperties>
#include <QFrame>
#include <QPropertyAnimation>
#include <QEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QPixmap>
#include <QPainter>
#include <QColor>
#include <QIcon>

#include <algorithm>
#include <initializer_list>

namespace {
// A 14px status dot: a filled green circle when active, otherwise transparent.
// Used both as a live indicator and (transparent) to reserve the dot column so
// every settings item lines up.
QIcon statusDotIcon(bool active) {
    QPixmap pm(14, 14);
    pm.fill(Qt::transparent);
    if (active) {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(40, 170, 70));   // green
        p.drawEllipse(QPointF(7, 7), 5, 5);
    }
    return QIcon(pm);
}

// Header nav-button glyphs, drawn rather than shipped as assets so they pick up
// the menu's title-foreground colour and stay crisp at the button size. A close
// "✕" on the main page, a back chevron "‹" on the settings sub-page.
QIcon closeIcon(const QColor& c) {
    QPixmap pm(24, 24);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c);
    pen.setWidthF(2.2);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.drawLine(7, 7, 17, 17);
    p.drawLine(17, 7, 7, 17);
    return QIcon(pm);
}

QIcon backIcon(const QColor& c) {
    QPixmap pm(24, 24);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(c);
    pen.setWidthF(2.2);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.drawLine(15, 6, 9, 12);
    p.drawLine(9, 12, 15, 18);
    return QIcon(pm);
}
} // namespace

SideMenu::SideMenu(Settings* settings, QWidget* parent)
    : QWidget(parent), settings_(settings) {
    setVisible(false);

    // Scrim covers the whole chart; a tap anywhere on it closes the menu.
    scrim_ = new QWidget(this);
    scrim_->setStyleSheet(QStringLiteral("background: rgba(0,0,0,90);"));
    scrim_->installEventFilter(this);

    // Panel: the opaque menu surface that slides in from the left edge.
    const theme::MenuPalette& th = theme::menu();
    panel_ = new QWidget(this);
    panel_->setStyleSheet(QStringLiteral("background:%1; border-right:1px solid %2;")
                          .arg(th.panelBg, th.panelBorder));

    auto* outer = new QVBoxLayout(panel_);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Header bar: a nav button (close "✕" on the main page, back "‹" on the
    // settings sub-page) plus the page title. Keeping the dismiss/back affordance
    // here means it is always reachable without scrolling to the bottom.
    auto* header = new QWidget(panel_);
    header->setStyleSheet(QStringLiteral("background:%1;").arg(th.titleBg));
    auto* headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(8, 8, 16, 8);
    headerRow->setSpacing(6);

    navBtn_ = new QPushButton(header);
    navBtn_->setFlat(true);
    navBtn_->setFixedSize(44, 44);                 // comfortable touch target
    navBtn_->setIconSize(QSize(24, 24));
    navBtn_->setCursor(Qt::PointingHandCursor);
    navBtn_->setStyleSheet(QStringLiteral(
        "QPushButton{ border:none; background:transparent; }"
        "QPushButton:pressed{ background:%1; border-radius:6px; }").arg(th.actionPressed));
    // Context-sensitive: on any sub-page it goes back to the main page,
    // otherwise it closes the menu.
    connect(navBtn_, &QPushButton::clicked, this, [this] {
        if (stack_->currentIndex() != mainIndex_) showMainPage();
        else                                      closeMenu();
    });
    headerRow->addWidget(navBtn_);

    title_ = new QLabel(QStringLiteral("Menu"), header);
    title_->setStyleSheet(QStringLiteral(
        "font-size:18px; font-weight:600; background:transparent; color:%1;")
        .arg(th.titleFg));
    headerRow->addWidget(title_, 1);

    outer->addWidget(header);

    stack_ = new QStackedWidget(panel_);
    // Build the pages first, then widen the panel to fit the widest item's full
    // text. Sizing to the content leaves the scroll areas with no horizontal
    // range, so the menu only ever scrolls up/down (no side-to-side drag) while
    // still showing every label without truncation or horizontal scrolling.
    QWidget* mainPage     = buildMainPage();
    QWidget* settingsPage = buildSettingsPage();
    QWidget* routesPage   = buildRoutesPage();
    int contentW = 0;
    for (QWidget* p : { mainPage, settingsPage, routesPage }) {
        p->ensurePolished();   // apply stylesheet metrics before measuring
        contentW = std::max(contentW, p->sizeHint().width());
    }
    // Allow for the vertical scrollbar plus a little right-edge breathing space,
    // and never go narrower than the original default width.
    constexpr int kScrollbarAllowance = 24;
    panelWidth_ = std::max(panelWidth_, contentW + kScrollbarAllowance);

    mainIndex_     = stack_->addWidget(wrapScroll(mainPage));
    settingsIndex_ = stack_->addWidget(wrapScroll(settingsPage));
    routesIndex_   = stack_->addWidget(wrapScroll(routesPage));
    outer->addWidget(stack_, 1);

    anim_ = new QPropertyAnimation(panel_, "geometry", this);
    anim_->setDuration(160);
    connect(anim_, &QPropertyAnimation::finished, this, [this] {
        if (!open_) setVisible(false);   // hide overlay once the slide-out ends
    });

    // Keep the chart-set list (and its active highlight) current.
    connect(settings_, &Settings::chartSetsChanged, this, &SideMenu::rebuildChartSets);
    connect(settings_, &Settings::chartDirectoryChanged,
            this, [this](const QString&) { rebuildChartSets(); });

    if (parent) parent->installEventFilter(this);
}

// ---- page construction ----------------------------------------------------

QWidget* SideMenu::buildMainPage() {
    auto* page = new QWidget;
    auto* col = new QVBoxLayout(page);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    col->addWidget(makeHeader(QStringLiteral("Chart Sets")));
    auto* setsHolder = new QWidget(page);
    chartSetsBox_ = new QVBoxLayout(setsHolder);
    chartSetsBox_->setContentsMargins(0, 0, 0, 0);
    chartSetsBox_->setSpacing(0);
    col->addWidget(setsHolder);
    rebuildChartSets();

    // Raster/imagery toggles live with the chart sets: they pick which chart
    // data is drawn, so they belong next to the set selection rather than under
    // the vector-only Chart Detail toggles.
    //
    // For now the Raster Charts item is hidden and raster is forced on: flip
    // kShowRasterChartsItem back to true to expose the user toggle again.
    constexpr bool kShowRasterChartsItem = false;
    if (kShowRasterChartsItem) {
        auto* raster = makeCheckAction(QStringLiteral("Raster Charts"), settings_->showRasterCharts());
        connect(raster, &QPushButton::toggled, settings_, &Settings::setShowRasterCharts);
        col->addWidget(raster);
    } else if (!settings_->showRasterCharts()) {
        settings_->setShowRasterCharts(true);   // keep raster on while hidden
    }
    auto* overlay = makeCheckAction(QStringLiteral("Vectors over Imagery"), settings_->vectorOverlay());
    connect(overlay, &QPushButton::toggled, settings_, &Settings::setVectorOverlay);
    col->addWidget(overlay);

    col->addWidget(makeHeader(QStringLiteral("Chart Detail")));
    auto* detailLvlBtn = makeIndentedAction(QStringLiteral("Detail Level"));
    connect(detailLvlBtn, &QPushButton::clicked, this, [this] {
        emit editChartDetailLevelRequested();
    });
    col->addWidget(detailLvlBtn);
    auto* snd = makeCheckAction(QStringLiteral("Soundings"), settings_->showSoundings());
    connect(snd, &QPushButton::toggled, settings_, &Settings::setShowSoundings);
    col->addWidget(snd);
    auto* sym = makeCheckAction(QStringLiteral("Symbols"), settings_->showSymbols());
    connect(sym, &QPushButton::toggled, settings_, &Settings::setShowSymbols);
    col->addWidget(sym);
    auto* txt = makeCheckAction(QStringLiteral("Text"), settings_->showText());
    connect(txt, &QPushButton::toggled, settings_, &Settings::setShowText);
    col->addWidget(txt);
    auto* con = makeCheckAction(QStringLiteral("Depth Contours"), settings_->showDepthContours());
    connect(con, &QPushButton::toggled, settings_, &Settings::setShowDepthContours);
    col->addWidget(con);

    col->addWidget(makeHeader(QStringLiteral("View")));
    auto* centerBtn = makeIndentedAction(QStringLiteral("Center on Own Ship"));
    connect(centerBtn, &QPushButton::clicked, this, [this] {
        emit centerOnOwnshipRequested();
        if (autoHide_) closeMenu();
    });
    col->addWidget(centerBtn);

    auto* zoomChartsBtn = makeIndentedAction(QStringLiteral("Zoom to Charts"));
    connect(zoomChartsBtn, &QPushButton::clicked, this, [this] {
        emit zoomToChartsRequested();
        if (autoHide_) closeMenu();
    });
    col->addWidget(zoomChartsBtn);

    autoFollowBtn_ = makeCheckAction(QStringLiteral("Auto Follow"), false);
    connect(autoFollowBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit autoFollowToggled(on); });
    col->addWidget(autoFollowBtn_);

    auto* aisShow = makeCheckAction(QStringLiteral("AIS Targets"),
                                    settings_->showAisTargets());
    connect(aisShow, &QPushButton::toggled, settings_, &Settings::setShowAisTargets);
    col->addWidget(aisShow);

    auto* aisListBtn = makeIndentedAction(QStringLiteral("List AIS Targets"));
    connect(aisListBtn, &QPushButton::clicked, this, [this] {
        emit aisTargetListRequested();
        if (autoHide_) closeMenu();
    });
    col->addWidget(aisListBtn);

    col->addWidget(makeHeader(QStringLiteral("Navigation")));
    auto* routesBtn = makeIndentedAction(QStringLiteral("Routes and Waypoints"));
    connect(routesBtn, &QPushButton::clicked, this, &SideMenu::showRoutesPage);
    col->addWidget(routesBtn);
    auto* navOptsBtn = makeIndentedAction(QStringLiteral("Navigation Options"));
    connect(navOptsBtn, &QPushButton::clicked, this, [this] {
        emit navigationOptionsRequested();
        if (autoHide_) closeMenu();
    });
    col->addWidget(navOptsBtn);

    navigatingBtn_ = makeCheckAction(QStringLiteral("Navigating"), false);
    connect(navigatingBtn_, &QPushButton::toggled, this,
            [this](bool on) { emit navigatingToggled(on); });
    col->addWidget(navigatingBtn_);

    // Plugins section: hidden until a plugin contributes its first item.
    pluginHeader_ = makeHeader(QStringLiteral("Plugins"));
    pluginHeader_->setVisible(false);
    col->addWidget(pluginHeader_);
    auto* pluginHolder = new QWidget(page);
    pluginBox_ = new QVBoxLayout(pluginHolder);
    pluginBox_->setContentsMargins(0, 0, 0, 0);
    pluginBox_->setSpacing(0);
    col->addWidget(pluginHolder);

    col->addStretch(1);

    col->addWidget(makeHeader(QStringLiteral("System")));
    auto* settingsBtn = makeIndentedAction(QStringLiteral("Settings"));
    connect(settingsBtn, &QPushButton::clicked, this, &SideMenu::showSettingsPage);
    col->addWidget(settingsBtn);
    auto* aboutBtn = makeIndentedAction(QStringLiteral("About…"));
    connect(aboutBtn, &QPushButton::clicked, this, [this] {
        emit aboutRequested();
        if (autoHide_) closeMenu();
    });
    col->addWidget(aboutBtn);
    // Dismiss now lives on the header bar (the "✕" button), always reachable.

    return page;
}

QWidget* SideMenu::buildSettingsPage() {
    auto* page = new QWidget;
    auto* col = new QVBoxLayout(page);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    col->addWidget(makeHeader(QStringLiteral("Ships")));
    auto* predBtn = makeSettingsAction(QStringLiteral("Course Prediction Line…"));
    connect(predBtn, &QPushButton::clicked, this,
            [this] { emit editOwnshipPredictionRequested(); });
    col->addWidget(predBtn);

    auto* shipSizeBtn = makeSettingsAction(QStringLiteral("Ship Size…"));
    connect(shipSizeBtn, &QPushButton::clicked, this,
            [this] { emit editVesselSizeRequested(); });
    col->addWidget(shipSizeBtn);

    auto* mmsiBtn = makeSettingsAction(QStringLiteral("Own Ship MMSI…"));
    connect(mmsiBtn, &QPushButton::clicked, this,
            [this] { emit editOwnshipMmsiRequested(); });
    col->addWidget(mmsiBtn);

    auto* headingSrcBtn = makeSettingsAction(QStringLiteral("Heading Source…"));
    connect(headingSrcBtn, &QPushButton::clicked, this,
            [this] { emit editHeadingSourceRequested(); });
    col->addWidget(headingSrcBtn);

    auto* dangerBtn = makeSettingsAction(QStringLiteral("Dangerous Ships…"));
    connect(dangerBtn, &QPushButton::clicked, this,
            [this] { emit editDangerousShipsRequested(); });
    col->addWidget(dangerBtn);

    col->addWidget(makeHeader(QStringLiteral("Charts")));
    auto* chartSetsBtn = makeSettingsAction(QStringLiteral("Chart Sets"));
    connect(chartSetsBtn, &QPushButton::clicked, this,
            [this] { emit manageChartSetsRequested(); });
    col->addWidget(chartSetsBtn);

    auto* symSizeBtn = makeSettingsAction(QStringLiteral("Symbol Size…"));
    connect(symSizeBtn, &QPushButton::clicked, this,
            [this] { emit editSymbolSizeRequested(); });
    col->addWidget(symSizeBtn);

    auto* hideSymPanBtn = makeCheckAction(QStringLiteral("Hide Symbols during pan/zoom"),
                                          settings_->hideSymbolsWhilePanning());
    connect(hideSymPanBtn, &QPushButton::toggled,
            settings_, &Settings::setHideSymbolsWhilePanning);
    connect(settings_, &Settings::hideSymbolsWhilePanningChanged, hideSymPanBtn,
            [hideSymPanBtn](bool on) {
        if (hideSymPanBtn->isChecked() != on) hideSymPanBtn->setChecked(on);
    });
    col->addWidget(hideSymPanBtn);

    auto* basemapBtn = makeSettingsAction(QStringLiteral("Basemap Folder…"));
    connect(basemapBtn, &QPushButton::clicked, this,
            [this] { emit basemapFolderRequested(); });
    col->addWidget(basemapBtn);

    auto* unitsBtn = makeSettingsAction(QStringLiteral("Units…"));
    connect(unitsBtn, &QPushButton::clicked, this,
            [this] { emit editUnitsRequested(); });
    col->addWidget(unitsBtn);

    // Plugin settings pages: hidden until a plugin contributes one.
    pluginSettingsHeader_ = makeHeader(QStringLiteral("Plugin Settings"));
    pluginSettingsHeader_->setVisible(false);
    col->addWidget(pluginSettingsHeader_);
    auto* psHolder = new QWidget(page);
    pluginSettingsBox_ = new QVBoxLayout(psHolder);
    pluginSettingsBox_->setContentsMargins(0, 0, 0, 0);
    pluginSettingsBox_->setSpacing(0);
    col->addWidget(psHolder);

    col->addWidget(makeHeader(QStringLiteral("Data Connections")));

    // Data sources (NMEA 0183/2000 and plugin-registered sources) register
    // themselves into dataSourceBox_ below, each with its own status dot.
    auto* dsHolder = new QWidget(page);
    dataSourceBox_ = new QVBoxLayout(dsHolder);
    dataSourceBox_->setContentsMargins(0, 0, 0, 0);
    dataSourceBox_->setSpacing(0);
    col->addWidget(dsHolder);

    // --- Separator, then the tools that act on the connections ---
    col->addWidget(makeSeparator());

    auto* priorityBtn = makeSettingsAction(QStringLiteral("Data Priority"));
    connect(priorityBtn, &QPushButton::clicked, this,
            [this] { emit editDataPriorityRequested(); });
    col->addWidget(priorityBtn);

    auto* staleBtn = makeSettingsAction(QStringLiteral("Stale Data Thresholds…"));
    connect(staleBtn, &QPushButton::clicked, this,
            [this] { emit editStaleThresholdsRequested(); });
    col->addWidget(staleBtn);

    auto* navBrowserBtn = makeSettingsAction(QStringLiteral("NavData Browser"));
    connect(navBrowserBtn, &QPushButton::clicked, this,
            [this] { emit navDataBrowserRequested(); });
    col->addWidget(navBrowserBtn);

    col->addWidget(makeHeader(QStringLiteral("Menu")));
    auto* autoHideBtn = makeCheckAction(QStringLiteral("Auto Hide Menu"),
                                        settings_->autoHideMenu());
    connect(autoHideBtn, &QPushButton::toggled, settings_, &Settings::setAutoHideMenu);
    // Mirror external changes (no-ops a click-driven change).
    connect(settings_, &Settings::autoHideMenuChanged, autoHideBtn, [autoHideBtn](bool on) {
        if (autoHideBtn->isChecked() != on) autoHideBtn->setChecked(on);
    });
    col->addWidget(autoHideBtn);

    col->addStretch(1);
    // Back now lives on the header bar (the "‹" button), always reachable.

    return page;
}

QWidget* SideMenu::buildRoutesPage() {
    auto* page = new QWidget;
    auto* col = new QVBoxLayout(page);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    // Lists are the management hub (visibility, properties, delete, edit on
    // chart). "+" button on the chart and long-press on empty water cover most
    // creates without a menu trip, so this page stays short: New Route, Drop
    // Waypoint at Boat, and the combined Routes & Waypoints browser. Tap a saved
    // object on the chart to get rename / drag / properties / hide / delete.
    auto* createRoute = makeIndentedAction(QStringLiteral("New Route"));
    connect(createRoute, &QPushButton::clicked, this, [this] { emit createRouteRequested(); });
    col->addWidget(createRoute);
    auto* dropWpt = makeIndentedAction(QStringLiteral("Drop Waypoint at Boat"));
    connect(dropWpt, &QPushButton::clicked, this, [this] { emit dropWaypointRequested(); });
    col->addWidget(dropWpt);
    auto* listBtn = makeIndentedAction(QStringLiteral("Routes & Waypoints…"));
    connect(listBtn, &QPushButton::clicked, this, [this] { emit routeWaypointListRequested(); });
    col->addWidget(listBtn);

    col->addStretch(1);
    return page;
}

QWidget* SideMenu::wrapScroll(QWidget* content) {
    // Let a page scroll when its items are taller than the panel, so the content
    // is always sized by its items (never squeezed by the layout) and the list
    // can grow as more chart sets are added.
    auto* area = new QScrollArea;
    area->setWidget(content);
    area->setWidgetResizable(true);      // size content to items; scroll if taller
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget"
                                       "{ background:%1; }").arg(theme::menu().panelBg));
    // Touch: drag anywhere to scroll (kinetic). Taps still hit the buttons.
    QScroller::grabGesture(area->viewport(), QScroller::LeftMouseButtonGesture);
    // Vertical-only: the panel is sized to its content so there is no horizontal
    // range to scroll, and turning off horizontal overshoot removes any side-to-
    // side rubber-banding too, so dragging only ever moves the list up/down.
    QScroller* scroller = QScroller::scroller(area->viewport());
    QScrollerProperties props = scroller->scrollerProperties();
    props.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy,
                          QVariant::fromValue(QScrollerProperties::OvershootAlwaysOff));
    scroller->setScrollerProperties(props);
    return area;
}

void SideMenu::rebuildChartSets() {
    if (!chartSetsBox_) return;

    // Drop the existing set buttons before rebuilding from current settings.
    while (QLayoutItem* item = chartSetsBox_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    const QVector<ChartSet>& sets = settings_->chartSets();
    if (sets.isEmpty()) {
        auto* empty = new QLabel(
            QStringLiteral("No chart sets yet.\nAdd one in Settings → Chart Sets."));
        empty->setWordWrap(true);
        empty->setStyleSheet(QStringLiteral("color:%1; padding:14px 24px; font-size:14px;")
                              .arg(theme::menu().hint));
        chartSetsBox_->addWidget(empty);
        return;
    }

    const QString active = settings_->chartDirectory();
    for (const ChartSet& cs : sets) {
        const bool isActive = (cs.directory == active);
        auto* b = makeAction((isActive ? QStringLiteral("✓  ")    // check mark
                                        : QStringLiteral("     ")) + cs.name);
        b->setToolTip(cs.directory);
        if (isActive)
            b->setStyleSheet(b->styleSheet() +
                QStringLiteral("QPushButton{ color:%1; font-weight:600; }")
                    .arg(theme::menu().accent));
        const QString dir = cs.directory;
        connect(b, &QPushButton::clicked, this, [this, dir] {
            emit chartSetSelected(dir);
            if (autoHide_) closeMenu();
        });
        chartSetsBox_->addWidget(b);
    }
}

void SideMenu::showMainPage() {
    stack_->setCurrentIndex(mainIndex_);
    title_->setText(QStringLiteral("Menu"));
    navBtn_->setIcon(closeIcon(QColor(theme::menu().titleFg)));
    navBtn_->setToolTip(QStringLiteral("Close menu"));
}

void SideMenu::showSettingsPage() {
    stack_->setCurrentIndex(settingsIndex_);
    title_->setText(QStringLiteral("Settings"));
    navBtn_->setIcon(backIcon(QColor(theme::menu().titleFg)));
    navBtn_->setToolTip(QStringLiteral("Back"));
}

void SideMenu::showRoutesPage() {
    stack_->setCurrentIndex(routesIndex_);
    title_->setText(QStringLiteral("Routes and Waypoints"));
    navBtn_->setIcon(backIcon(QColor(theme::menu().titleFg)));
    navBtn_->setToolTip(QStringLiteral("Back"));
}

// ---- widget factories -----------------------------------------------------

QLabel* SideMenu::makeHeader(const QString& text) {
    const theme::MenuPalette& t = theme::menu();
    auto* h = new QLabel(text);
    h->setStyleSheet(QStringLiteral(
        "font-size:12px; font-weight:600; color:%1;"
        "padding:14px 20px 6px 20px; background:%2;").arg(t.headerFg, t.headerBg));
    return h;
}

QWidget* SideMenu::makeSeparator() {
    // A 1px rule inset from the edges, with a little breathing room above/below.
    const theme::MenuPalette& t = theme::menu();
    auto* wrap = new QWidget;
    wrap->setStyleSheet(QStringLiteral("background:%1;").arg(t.panelBg));
    auto* lay = new QVBoxLayout(wrap);
    lay->setContentsMargins(20, 6, 20, 6);
    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setStyleSheet(QStringLiteral("color:%1;").arg(t.separator));
    lay->addWidget(line);
    return wrap;
}

QPushButton* SideMenu::makeAction(const QString& text) {
    const theme::MenuPalette& t = theme::menu();
    auto* b = new QPushButton(text);
    b->setMinimumHeight(56);   // comfortable touch target
    b->setCursor(Qt::PointingHandCursor);
    // Pin both background AND text colour so the menu renders the same on a
    // dark-mode system (Qt's default palette would otherwise paint white text
    // on this hard-coded background and make items unreadable).
    b->setStyleSheet(QStringLiteral(
        "QPushButton{ text-align:left; padding-left:24px; border:none;"
        " font-size:16px; background:%1; color:%2; }"
        "QPushButton:pressed{ background:%3; }")
        .arg(t.actionBg, t.actionFg, t.actionPressed));
    return b;
}

QPushButton* SideMenu::makeSettingsAction(const QString& text) {
    auto* b = makeAction(text);
    b->setIconSize(QSize(14, 14));
    b->setIcon(statusDotIcon(false));   // transparent: reserves the dot column
    return b;
}

QPushButton* SideMenu::makeIndentedAction(const QString& text) {
    // Blank check-mark column (same width as the unchecked ✓ slot) so plain
    // items line up with the checkable ones.
    return makeAction(QStringLiteral("     ") + text);
}

QPushButton* SideMenu::makeCheckAction(const QString& text, bool checked) {
    auto* b = new QPushButton();
    b->setCheckable(true);
    b->setMinimumHeight(56);
    b->setCursor(Qt::PointingHandCursor);
    // Same check-mark cue as the active chart set: a tick when on, blank when
    // off, with the matching blue/bold accent.
    auto sync = [b, text](bool on) {
        b->setText((on ? QStringLiteral("✓  ") : QStringLiteral("     ")) + text);
    };
    sync(checked);
    b->setChecked(checked);
    connect(b, &QPushButton::toggled, b, sync);
    // Pin colours: see makeAction. Without the explicit normal-state color the
    // unchecked text would be white-on-white in dark mode.
    const theme::MenuPalette& t = theme::menu();
    b->setStyleSheet(QStringLiteral(
        "QPushButton{ text-align:left; padding-left:24px; border:none;"
        " font-size:16px; background:%1; color:%2; }"
        "QPushButton:checked{ color:%3; font-weight:600; }"
        "QPushButton:pressed{ background:%4; }")
        .arg(t.actionBg, t.actionFg, t.accent, t.actionPressed));
    return b;
}

void SideMenu::setAutoFollowChecked(bool on) {
    // Guard prevents a feedback loop (the view's setAutoFollow already no-ops on
    // an unchanged state, but this avoids the redundant toggle/text churn).
    if (autoFollowBtn_ && autoFollowBtn_->isChecked() != on)
        autoFollowBtn_->setChecked(on);
}

void SideMenu::setNavigatingChecked(bool on) {
    // Guard against the feedback loop: setChecked re-emits toggled, which the host
    // routes back here as activeChanged. Only touch the button on a real change.
    if (navigatingBtn_ && navigatingBtn_->isChecked() != on)
        navigatingBtn_->setChecked(on);
}

void SideMenu::addPluginAction(const QString& title, std::function<void()> onTriggered) {
    if (!pluginBox_) return;
    if (pluginHeader_) pluginHeader_->setVisible(true);
    auto* b = makeIndentedAction(title);
    connect(b, &QPushButton::clicked, this, [fn = std::move(onTriggered)] { if (fn) fn(); });
    pluginBox_->addWidget(b);
}

void SideMenu::addPluginToggle(const QString& title, bool checked,
                               std::function<void(bool)> onToggled) {
    if (!pluginBox_) return;
    if (pluginHeader_) pluginHeader_->setVisible(true);
    auto* b = makeCheckAction(title, checked);
    connect(b, &QPushButton::toggled, this,
            [fn = std::move(onToggled)](bool on) { if (fn) fn(on); });
    pluginBox_->addWidget(b);
}

QPushButton* SideMenu::addDataSourceItem(const QString& title, std::function<void()> onClicked) {
    if (!dataSourceBox_) return nullptr;
    auto* b = makeSettingsAction(title);   // reserves the status-dot column
    connect(b, &QPushButton::clicked, this, [fn = std::move(onClicked)] { if (fn) fn(); });
    dataSourceBox_->addWidget(b);
    return b;
}

void SideMenu::setItemDot(QPushButton* item, bool on) {
    if (item) item->setIcon(statusDotIcon(on));
}

void SideMenu::addPluginSettingsItem(const QString& title, std::function<void()> onClicked) {
    if (!pluginSettingsBox_) return;
    if (pluginSettingsHeader_) pluginSettingsHeader_->setVisible(true);
    auto* b = makeSettingsAction(title);
    connect(b, &QPushButton::clicked, this, [fn = std::move(onClicked)] { if (fn) fn(); });
    pluginSettingsBox_->addWidget(b);
}

// ---- open/close + geometry ------------------------------------------------

void SideMenu::openMenu() {
    if (open_) return;
    open_ = true;
    showMainPage();   // always start on the main page
    applyModeGeometry();   // full-parent + scrim, or panel-only strip
    panel_->setGeometry(-panelWidth_, 0, panelWidth_, height());
    panel_->raise();
    setVisible(true);
    raise();
    anim_->stop();
    anim_->setStartValue(panel_->geometry());
    anim_->setEndValue(QRect(0, 0, panelWidth_, height()));
    anim_->start();
}

void SideMenu::closeMenu() {
    if (!open_) return;
    open_ = false;
    anim_->stop();
    anim_->setStartValue(panel_->geometry());
    anim_->setEndValue(QRect(-panelWidth_, 0, panelWidth_, height()));
    anim_->start();
}

void SideMenu::layoutPanel() {
    scrim_->setGeometry(rect());
    scrim_->lower();
    scrim_->setVisible(autoHide_);   // no scrim in "stay-open" mode
    if (anim_->state() != QAbstractAnimation::Running) {
        panel_->setGeometry(open_ ? QRect(0, 0, panelWidth_, height())
                                  : QRect(-panelWidth_, 0, panelWidth_, height()));
    }
    panel_->raise();
}

// Resize ourselves to match the current mode. autoHide=true → cover the parent
// fully so the scrim catches taps; autoHide=false → cover only the panel strip
// so mouse events outside the panel reach the chart underneath.
void SideMenu::applyModeGeometry() {
    if (!parentWidget()) return;
    if (autoHide_)
        setGeometry(parentWidget()->rect());
    else
        setGeometry(0, 0, panelWidth_, parentWidget()->height());
    layoutPanel();
}

void SideMenu::setAutoHide(bool on) {
    if (on == autoHide_) return;
    autoHide_ = on;
    if (isVisible()) applyModeGeometry();   // switch geometry/scrim live
}

void SideMenu::resizeEvent(QResizeEvent*) {
    layoutPanel();
}

// Swallow wheel events that bubble up to us. A page's QScrollArea only consumes
// the wheel when it actually has room to scroll; when the content fits, the
// unhandled event would otherwise propagate to the chart view underneath and
// zoom/pan it. Accepting it here stops that while leaving real scrolling intact.
void SideMenu::wheelEvent(QWheelEvent* e) {
    e->accept();
}

bool SideMenu::eventFilter(QObject* obj, QEvent* e) {
    if (obj == parentWidget() && e->type() == QEvent::Resize) {
        if (isVisible()) applyModeGeometry();
    } else if (obj == scrim_ && e->type() == QEvent::MouseButtonPress) {
        if (autoHide_) closeMenu();   // tap-outside dismiss only in auto-hide mode
        return true;
    }
    return QWidget::eventFilter(obj, e);
}
