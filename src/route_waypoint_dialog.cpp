#include "route_waypoint_dialog.hpp"
#include "route_store.hpp"
#include "units.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QScroller>
#include <QPushButton>
#include <QCheckBox>
#include <QFrame>
#include <QLabel>
#include <QSizePolicy>
#include <QTabWidget>
#include <QMessageBox>

namespace {
// A transparent-to-mouse cell label (taps fall through to the row button).
QLabel* makeCell(int fixedWidth, Qt::Alignment align) {
    auto* l = new QLabel;
    l->setAttribute(Qt::WA_TransparentForMouseEvents);
    l->setAlignment(align);
    l->setStyleSheet(QStringLiteral("font-size:14px; padding:0 4px; border:none; color:%1;")
                     .arg(theme::menu().actionFg));
    if (fixedWidth > 0) l->setFixedWidth(fixedWidth);
    return l;
}

QString fmtPos(double lat, double lon) {
    return units::formatLatitude(lat) + QStringLiteral("  ") + units::formatLongitude(lon);
}

// Column-header strip shared by both list panels.
QWidget* makeHeaderStrip(const QString& objName,
                         const QString& secondCol, int secondWidth) {
    const theme::MenuPalette& t = theme::menu();
    auto* hdr = new QWidget;
    hdr->setObjectName(objName);
    hdr->setStyleSheet(QStringLiteral("#%1{ background:%2; border-bottom:1px solid %3; }")
                           .arg(objName, t.headerBg, t.separator));
    auto* hl = new QHBoxLayout(hdr);
    hl->setContentsMargins(8, 4, 8, 4);
    hl->setSpacing(0);
    auto headerCell = [&](const QString& text, int width, Qt::Alignment align) {
        auto* l = new QLabel(text, hdr);
        l->setStyleSheet(QStringLiteral(
            "font-size:12px; font-weight:600; color:%1; padding:0 4px;").arg(t.headerFg));
        if (width > 0) l->setFixedWidth(width);
        else l->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        l->setAlignment(align);
        return l;
    };
    hl->addWidget(headerCell(QStringLiteral("Name"),    0,           Qt::AlignLeft  | Qt::AlignVCenter), 1);
    hl->addWidget(headerCell(secondCol,                 secondWidth, Qt::AlignRight | Qt::AlignVCenter));
    hl->addWidget(headerCell(QStringLiteral("Visible"), 64,          Qt::AlignCenter));
    return hdr;
}

// Scrollable, drag-to-scroll row container (vertical only), matching the AIS list.
QScrollArea* makeRowScroll(QWidget*& rowContainerOut, QVBoxLayout*& rowLayoutOut) {
    const theme::MenuPalette& t = theme::menu();
    auto* scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea, QScrollArea > QWidget > QWidget { background:%1; }").arg(t.panelBg));
    auto* container = new QWidget;
    auto* lay = new QVBoxLayout(container);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addStretch(1);
    scroll->setWidget(container);
    QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);
    rowContainerOut = container;
    rowLayoutOut = lay;
    return scroll;
}

// Selectable list-row button, with the accent highlight when checked.
QPushButton* makeRowButton(QWidget* parent) {
    const theme::MenuPalette& t = theme::menu();
    auto* b = new QPushButton(parent);
    b->setFlat(true);
    b->setMinimumHeight(44);
    b->setCheckable(true);
    b->setStyleSheet(QStringLiteral(
        "QPushButton { text-align:left; border:none; background:%1; color:%2;"
        " border-bottom:1px solid %3; }"
        "QPushButton:checked { background:%4; color:%5; }")
        .arg(t.actionBg, t.actionFg, t.separator, t.accent, t.titleFg));
    return b;
}
}  // namespace

// ==== RouteListPanel =======================================================

RouteListPanel::RouteListPanel(RouteStore* store, bool pickMode, QWidget* parent)
    : QWidget(parent), store_(store), pickMode_(pickMode) {
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    col->addWidget(makeHeaderStrip(QStringLiteral("RouteListHdr"),
                                   QStringLiteral("Points"), 70));
    scrollArea_ = makeRowScroll(rowContainer_, rowLayout_);
    col->addWidget(scrollArea_, 1);

    // Action bar (no Close — the dialog owns dismissal). Hidden entirely in pick mode.
    auto* btnBar = new QWidget;
    auto* btnRow = new QHBoxLayout(btnBar);
    btnRow->setContentsMargins(12, 8, 12, 12);
    btnRow->setSpacing(8);
    deleteBtn_ = new QPushButton(QStringLiteral("Delete"), btnBar);
    deleteBtn_->setEnabled(false);
    dialogchrome::styleOutlinedButton(deleteBtn_);
    connect(deleteBtn_, &QPushButton::clicked, this, &RouteListPanel::deleteSelected);
    btnRow->addWidget(deleteBtn_);
    propsBtn_ = new QPushButton(QStringLiteral("Properties"), btnBar);
    propsBtn_->setEnabled(false);
    dialogchrome::styleOutlinedButton(propsBtn_);
    connect(propsBtn_, &QPushButton::clicked, this, [this] {
        if (selectedId_ >= 0) emit propertiesRequested(selectedId_);
    });
    btnRow->addWidget(propsBtn_);
    editBtn_ = new QPushButton(QStringLiteral("Edit on Chart"), btnBar);
    editBtn_->setEnabled(false);
    dialogchrome::styleOutlinedButton(editBtn_);
    connect(editBtn_, &QPushButton::clicked, this, [this] {
        if (selectedId_ >= 0) emit editRequested(selectedId_);
    });
    btnRow->addWidget(editBtn_);
    btnRow->addStretch(1);
    newBtn_ = new QPushButton(QStringLiteral("New"), btnBar);
    dialogchrome::styleOutlinedButton(newBtn_);
    connect(newBtn_, &QPushButton::clicked, this, [this] { emit newRouteRequested(); });
    btnRow->addWidget(newBtn_);
    col->addWidget(btnBar);
    btnBar->setVisible(!pickMode_);

    if (store_) connect(store_, &RouteStore::routesChanged, this, &RouteListPanel::refresh);
    refresh();
}

RouteListPanel::Row RouteListPanel::makeRow() {
    Row r;
    r.btn = makeRowButton(rowContainer_);
    auto* hl = new QHBoxLayout(r.btn);
    hl->setContentsMargins(8, 0, 8, 0);
    hl->setSpacing(0);
    r.name = makeCell(0,  Qt::AlignLeft  | Qt::AlignVCenter);
    r.name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    r.meta = makeCell(70, Qt::AlignRight | Qt::AlignVCenter);
    hl->addWidget(r.name, 1);
    hl->addWidget(r.meta);

    // Visible checkbox stays clickable so it toggles independently of selecting.
    r.vis = new QCheckBox(r.btn);
    r.vis->setFixedWidth(64);
    r.vis->setStyleSheet(QStringLiteral("margin-left:24px;"));
    hl->addWidget(r.vis);

    connect(r.btn, &QPushButton::clicked, this, [this, btn = r.btn] {
        const qint64 id = btn->property("rid").toLongLong();
        if (pickMode_) emit routePicked(id);
        else           selectRow(id);
    });
    connect(r.vis, &QCheckBox::toggled, this, [this, box = r.vis](bool on) {
        const qint64 id = box->property("rid").toLongLong();
        if (store_) store_->setRouteVisible(id, on);
    });
    return r;
}

void RouteListPanel::refresh() {
    if (!store_) return;
    const QVector<Route>& routes = store_->routes();

    const int want = routes.size();
    while (int(rows_.size()) < want) {
        Row r = makeRow();
        rowLayout_->insertWidget(rowLayout_->count() - 1, r.btn);
        rows_.push_back(r);
    }
    while (int(rows_.size()) > want) {
        rows_.back().btn->deleteLater();
        rows_.pop_back();
    }

    bool selectionStillExists = false;
    for (int i = 0; i < want; ++i) {
        const Route& rt = routes[i];
        Row& row = rows_[i];
        row.id = rt.id;
        row.btn->setProperty("rid", QVariant::fromValue(rt.id));
        row.vis->setProperty("rid", QVariant::fromValue(rt.id));
        const QString nm = rt.name.isEmpty() ? QStringLiteral("(unnamed)") : rt.name;
        if (row.name->text() != nm) row.name->setText(nm);
        row.meta->setText(QString::number(rt.points.size()));
        row.vis->blockSignals(true);
        row.vis->setChecked(rt.visible);
        row.vis->blockSignals(false);
        if (rt.id == selectedId_) selectionStillExists = true;
    }
    if (!selectionStillExists) selectedId_ = -1;
    restyleRows();
    updateActionState();
}

void RouteListPanel::selectRow(qint64 id) {
    selectedId_ = (selectedId_ == id) ? -1 : id;   // tap again to deselect
    restyleRows();
    updateActionState();
}

void RouteListPanel::restyleRows() {
    const theme::MenuPalette& t = theme::menu();
    for (Row& r : rows_) {
        const bool sel = (r.id == selectedId_ && selectedId_ >= 0);
        r.btn->setChecked(sel);
        const QString cell = QStringLiteral(
            "font-size:14px; padding:0 4px; border:none; color:%1;")
            .arg(sel ? t.titleFg : t.actionFg);
        r.name->setStyleSheet(cell);
        r.meta->setStyleSheet(cell);
    }
}

void RouteListPanel::updateActionState() {
    const bool hasSel = selectedId_ >= 0;
    deleteBtn_->setEnabled(hasSel);
    propsBtn_->setEnabled(hasSel);
    editBtn_->setEnabled(hasSel);
}

void RouteListPanel::deleteSelected() {
    if (!store_ || selectedId_ < 0) return;
    const Route* r = store_->route(selectedId_);
    const QString nm = (r && !r->name.isEmpty()) ? r->name : QStringLiteral("this route");
    if (QMessageBox::question(this, QStringLiteral("Delete Route"),
            QStringLiteral("Delete %1?").arg(nm)) != QMessageBox::Yes)
        return;
    const qint64 id = selectedId_;
    selectedId_ = -1;
    store_->removeRoute(id);   // emits routesChanged -> refresh()
}

// ==== WaypointListPanel ====================================================

WaypointListPanel::WaypointListPanel(RouteStore* store, bool pickMode, QWidget* parent)
    : QWidget(parent), store_(store), pickMode_(pickMode) {
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    col->addWidget(makeHeaderStrip(QStringLiteral("WaypointListHdr"),
                                   QStringLiteral("Position"), 250));
    scrollArea_ = makeRowScroll(rowContainer_, rowLayout_);
    col->addWidget(scrollArea_, 1);

    auto* btnBar = new QWidget;
    auto* btnRow = new QHBoxLayout(btnBar);
    btnRow->setContentsMargins(12, 8, 12, 12);
    btnRow->setSpacing(8);
    deleteBtn_ = new QPushButton(QStringLiteral("Delete"), btnBar);
    deleteBtn_->setEnabled(false);
    dialogchrome::styleOutlinedButton(deleteBtn_);
    connect(deleteBtn_, &QPushButton::clicked, this, &WaypointListPanel::deleteSelected);
    btnRow->addWidget(deleteBtn_);
    propsBtn_ = new QPushButton(QStringLiteral("Properties"), btnBar);
    propsBtn_->setEnabled(false);
    dialogchrome::styleOutlinedButton(propsBtn_);
    connect(propsBtn_, &QPushButton::clicked, this, [this] {
        if (selectedId_ >= 0) emit propertiesRequested(selectedId_);
    });
    btnRow->addWidget(propsBtn_);
    editBtn_ = new QPushButton(QStringLiteral("Move on Chart"), btnBar);
    editBtn_->setEnabled(false);
    dialogchrome::styleOutlinedButton(editBtn_);
    connect(editBtn_, &QPushButton::clicked, this, [this] {
        if (selectedId_ >= 0) emit editRequested(selectedId_);
    });
    btnRow->addWidget(editBtn_);
    btnRow->addStretch(1);
    dropBtn_ = new QPushButton(QStringLiteral("Drop at Boat"), btnBar);
    dialogchrome::styleOutlinedButton(dropBtn_);
    connect(dropBtn_, &QPushButton::clicked, this, [this] { emit newWaypointAtOwnshipRequested(); });
    btnRow->addWidget(dropBtn_);
    col->addWidget(btnBar);
    btnBar->setVisible(!pickMode_);

    if (store_) connect(store_, &RouteStore::waypointsChanged, this, &WaypointListPanel::refresh);
    refresh();
}

WaypointListPanel::Row WaypointListPanel::makeRow() {
    Row r;
    r.btn = makeRowButton(rowContainer_);
    auto* hl = new QHBoxLayout(r.btn);
    hl->setContentsMargins(8, 0, 8, 0);
    hl->setSpacing(0);
    r.name = makeCell(0,   Qt::AlignLeft  | Qt::AlignVCenter);
    r.name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    r.pos  = makeCell(250, Qt::AlignRight | Qt::AlignVCenter);
    hl->addWidget(r.name, 1);
    hl->addWidget(r.pos);

    r.vis = new QCheckBox(r.btn);
    r.vis->setFixedWidth(64);
    r.vis->setStyleSheet(QStringLiteral("margin-left:24px;"));
    hl->addWidget(r.vis);

    connect(r.btn, &QPushButton::clicked, this, [this, btn = r.btn] {
        const qint64 id = btn->property("wid").toLongLong();
        if (pickMode_) emit waypointPicked(id);
        else           selectRow(id);
    });
    connect(r.vis, &QCheckBox::toggled, this, [this, box = r.vis](bool on) {
        const qint64 id = box->property("wid").toLongLong();
        if (store_) store_->setWaypointVisible(id, on);
    });
    return r;
}

void WaypointListPanel::refresh() {
    if (!store_) return;
    const QVector<Waypoint>& wpts = store_->waypoints();

    const int want = wpts.size();
    while (int(rows_.size()) < want) {
        Row r = makeRow();
        rowLayout_->insertWidget(rowLayout_->count() - 1, r.btn);
        rows_.push_back(r);
    }
    while (int(rows_.size()) > want) {
        rows_.back().btn->deleteLater();
        rows_.pop_back();
    }

    bool selectionStillExists = false;
    for (int i = 0; i < want; ++i) {
        const Waypoint& w = wpts[i];
        Row& row = rows_[i];
        row.id = w.id;
        row.btn->setProperty("wid", QVariant::fromValue(w.id));
        row.vis->setProperty("wid", QVariant::fromValue(w.id));
        const QString nm = w.name.isEmpty() ? QStringLiteral("(unnamed)") : w.name;
        if (row.name->text() != nm) row.name->setText(nm);
        row.pos->setText(fmtPos(w.lat, w.lon));
        row.vis->blockSignals(true);
        row.vis->setChecked(w.visible);
        row.vis->blockSignals(false);
        if (w.id == selectedId_) selectionStillExists = true;
    }
    if (!selectionStillExists) selectedId_ = -1;
    restyleRows();
    updateActionState();
}

void WaypointListPanel::selectRow(qint64 id) {
    selectedId_ = (selectedId_ == id) ? -1 : id;
    restyleRows();
    updateActionState();
}

void WaypointListPanel::restyleRows() {
    const theme::MenuPalette& t = theme::menu();
    for (Row& r : rows_) {
        const bool sel = (r.id == selectedId_ && selectedId_ >= 0);
        r.btn->setChecked(sel);
        const QString cell = QStringLiteral(
            "font-size:14px; padding:0 4px; border:none; color:%1;")
            .arg(sel ? t.titleFg : t.actionFg);
        r.name->setStyleSheet(cell);
        r.pos->setStyleSheet(cell);
    }
}

void WaypointListPanel::updateActionState() {
    const bool hasSel = selectedId_ >= 0;
    deleteBtn_->setEnabled(hasSel);
    propsBtn_->setEnabled(hasSel);
    editBtn_->setEnabled(hasSel);
}

void WaypointListPanel::deleteSelected() {
    if (!store_ || selectedId_ < 0) return;
    QString nm = QStringLiteral("this waypoint");
    for (const Waypoint& w : store_->waypoints())
        if (w.id == selectedId_) { if (!w.name.isEmpty()) nm = w.name; break; }
    if (QMessageBox::question(this, QStringLiteral("Delete Waypoint"),
            QStringLiteral("Delete %1?").arg(nm)) != QMessageBox::Yes)
        return;
    const qint64 id = selectedId_;
    selectedId_ = -1;
    store_->removeWaypoint(id);
}

// ==== RouteWaypointDialog ==================================================

namespace {
QString tabStyle() {
    const theme::MenuPalette& t = theme::menu();
    return QStringLiteral(
        "QTabWidget::pane{ border:1px solid %2; background:%1; }"
        "QTabBar::tab{ background:%3; color:%4; padding:8px 20px; border:none; font-size:14px; }"
        "QTabBar::tab:selected{ background:%1; color:%5; font-weight:600; }")
        .arg(t.panelBg, t.separator, t.headerBg, t.headerFg, t.actionFg);
}
}  // namespace

RouteWaypointDialog::RouteWaypointDialog(RouteStore* store, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle(QStringLiteral("Routes & Waypoints"));
    resize(560, 640);

    // Modeless + self-deleting in the host; the ✕ closes (destroys) the window.
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Routes & Waypoints"),
                                         /*closeOnDismiss=*/true);

    routePanel_ = new RouteListPanel(store_, /*pickMode=*/false);
    wptPanel_   = new WaypointListPanel(store_, /*pickMode=*/false);

    tabs_ = new QTabWidget;
    tabs_->setDocumentMode(true);
    tabs_->setStyleSheet(tabStyle());
    tabs_->addTab(routePanel_, QStringLiteral("Routes"));
    tabs_->addTab(wptPanel_,   QStringLiteral("Waypoints"));
    panelCol->addWidget(tabs_, 1);

    auto* btnBar = new QWidget;
    auto* row = new QHBoxLayout(btnBar);
    row->setContentsMargins(16, 8, 16, 16);
    row->addStretch(1);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"));
    dialogchrome::styleAccentButton(closeBtn);
    closeBtn->setDefault(true);
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);
    row->addWidget(closeBtn);
    panelCol->addWidget(btnBar);

    // Relay panel actions to the host. Edit / New / Drop also close the dialog
    // (the user is moving on to the chart); Properties leaves it open.
    connect(routePanel_, &RouteListPanel::propertiesRequested,
            this, &RouteWaypointDialog::routePropertiesRequested);
    connect(routePanel_, &RouteListPanel::editRequested, this, [this](qint64 id) {
        emit routeEditRequested(id); close();
    });
    connect(routePanel_, &RouteListPanel::newRouteRequested, this, [this] {
        emit newRouteRequested(); close();
    });
    connect(wptPanel_, &WaypointListPanel::propertiesRequested,
            this, &RouteWaypointDialog::waypointPropertiesRequested);
    connect(wptPanel_, &WaypointListPanel::editRequested, this, [this](qint64 id) {
        emit waypointEditRequested(id); close();
    });
    connect(wptPanel_, &WaypointListPanel::newWaypointAtOwnshipRequested, this, [this] {
        emit newWaypointAtOwnshipRequested(); close();
    });

    // Tab titles carry the live counts (gpx-style).
    auto refreshTitles = [this] {
        tabs_->setTabText(0, QStringLiteral("Routes (%1)").arg(store_->routes().size()));
        tabs_->setTabText(1, QStringLiteral("Waypoints (%1)").arg(store_->waypoints().size()));
    };
    if (store_) {
        connect(store_, &RouteStore::routesChanged,    this, [refreshTitles] { refreshTitles(); });
        connect(store_, &RouteStore::waypointsChanged, this, [refreshTitles] { refreshTitles(); });
        refreshTitles();
    }
}

RouteWaypointDialog::RouteWaypointDialog(RouteStore* store, Tab pickCategory, QWidget* parent)
    : QDialog(parent), store_(store) {
    const bool routes = (pickCategory == Tab::Routes);
    const QString title = routes ? QStringLiteral("Select Route")
                                  : QStringLiteral("Select Waypoint");
    setWindowTitle(title);
    resize(520, 600);

    // Modal picker; the ✕ cancels (reject).
    auto* panelCol = dialogchrome::setup(this, title, /*closeOnDismiss=*/false);
    if (routes) {
        routePanel_ = new RouteListPanel(store_, /*pickMode=*/true);
        panelCol->addWidget(routePanel_, 1);
        connect(routePanel_, &RouteListPanel::routePicked, this, [this](qint64 id) {
            pickedId_ = id; accept();
        });
    } else {
        wptPanel_ = new WaypointListPanel(store_, /*pickMode=*/true);
        panelCol->addWidget(wptPanel_, 1);
        connect(wptPanel_, &WaypointListPanel::waypointPicked, this, [this](qint64 id) {
            pickedId_ = id; accept();
        });
    }
}

void RouteWaypointDialog::selectTab(Tab t) {
    if (tabs_) tabs_->setCurrentIndex(int(t));
}

void RouteWaypointDialog::refreshLists() {
    if (routePanel_) routePanel_->refresh();
    if (wptPanel_)   wptPanel_->refresh();
}
