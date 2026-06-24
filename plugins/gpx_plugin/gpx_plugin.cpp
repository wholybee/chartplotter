#include "gpx_plugin.hpp"
#include "gpx_io.hpp"
#include "route_store.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QWidget>
#include <QTabWidget>
#include <QScrollArea>
#include <QScroller>
#include <QFrame>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDateTime>
#include <QSet>
#include <QPair>
#include <QVector>
#include <functional>
#include <memory>

namespace {

// A big touch target: tall, generous font, accent-filled as a primary action so
// it's easy to hit on a panel mounted at the helm.
QPushButton* bigButton(const QString& text) {
    const theme::MenuPalette& t = theme::menu();
    auto* b = new QPushButton(text);
    b->setMinimumHeight(60);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral(
        "QPushButton{ font-size:18px; padding:6px 16px; border:none;"
        " border-radius:6px; background:%1; color:%2; font-weight:600; }"
        "QPushButton:pressed{ background:%3; }")
        .arg(t.accent, t.titleFg, t.titleBg));
    return b;
}

// Small flat text action ("Select all" / "Clear") for a tab's toolbar.
QPushButton* linkButton(const QString& text) {
    const theme::MenuPalette& t = theme::menu();
    auto* b = new QPushButton(text);
    b->setCursor(Qt::PointingHandCursor);
    b->setFlat(true);
    b->setStyleSheet(QStringLiteral(
        "QPushButton{ border:none; background:transparent; color:%1;"
        " font-size:14px; font-weight:600; padding:4px 2px; }"
        "QPushButton:pressed{ color:%2; }")
        .arg(t.accent, t.actionFg));
    return b;
}

// A checkable list row styled like the side-menu check items: a leading ✓ when
// selected (blank, same width, when not), with the accent/bold cue on top.
QPushButton* makeCheckRow(const QString& text, bool checked) {
    const theme::MenuPalette& t = theme::menu();
    auto* b = new QPushButton();
    b->setCheckable(true);
    b->setMinimumHeight(48);
    b->setCursor(Qt::PointingHandCursor);
    // Keep the label position fixed as the tick appears/disappears (matches the
    // side menu: "✓  " on, five spaces off).
    auto sync = [b, text](bool on) {
        b->setText((on ? QStringLiteral("✓  ") : QStringLiteral("     ")) + text);
    };
    sync(checked);
    b->setChecked(checked);
    QObject::connect(b, &QPushButton::toggled, b, sync);
    b->setStyleSheet(QStringLiteral(
        "QPushButton{ text-align:left; padding-left:18px; border:none;"
        " border-bottom:1px solid %4; font-size:16px; background:%1; color:%2; }"
        "QPushButton:checked{ color:%3; font-weight:600; }"
        "QPushButton:pressed{ background:%5; }")
        .arg(t.actionBg, t.actionFg, t.accent, t.separator, t.actionPressed));
    return b;
}

// One export tab: a "Select all / Clear" toolbar over a scrolling checkable
// list. `rows` tracks the live row buttons (each carries a "cid" = item id);
// `populate` rebuilds the list from a fresh (id, label) set after an import.
struct SelTab {
    QWidget* page = nullptr;
    std::shared_ptr<QVector<QPushButton*>> rows;
    std::function<void(const QVector<QPair<qint64, QString>>&)> populate;
};

SelTab makeSelTab(const QString& noun) {
    const theme::MenuPalette& t = theme::menu();
    SelTab tab;
    tab.rows = std::make_shared<QVector<QPushButton*>>();

    auto* page = new QWidget;
    auto* pcol = new QVBoxLayout(page);
    pcol->setContentsMargins(0, 0, 0, 0);
    pcol->setSpacing(0);

    // Toolbar: bulk select / clear, on the section-header shade.
    auto* bar = new QWidget;
    bar->setStyleSheet(QStringLiteral("background:%1;").arg(t.headerBg));
    auto* brow = new QHBoxLayout(bar);
    brow->setContentsMargins(12, 4, 12, 4);
    brow->setSpacing(16);
    auto* allBtn  = linkButton(QStringLiteral("Select all"));
    auto* noneBtn = linkButton(QStringLiteral("Clear"));
    brow->addWidget(allBtn);
    brow->addWidget(noneBtn);
    brow->addStretch(1);
    pcol->addWidget(bar);

    auto* scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea, QScrollArea > QWidget > QWidget { background:%1; }")
        .arg(t.panelBg));
    auto* container = new QWidget;
    auto* list = new QVBoxLayout(container);
    list->setContentsMargins(0, 0, 0, 0);
    list->setSpacing(0);
    auto* empty = new QLabel(QStringLiteral("No %1 to export.").arg(noun));
    empty->setStyleSheet(QStringLiteral("font-size:14px; color:%1; padding:14px 16px;")
                             .arg(theme::textMuted()));
    list->addWidget(empty);
    list->addStretch(1);
    scroll->setWidget(container);
    QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);
    pcol->addWidget(scroll, 1);

    auto rows = tab.rows;
    QObject::connect(allBtn, &QPushButton::clicked, allBtn,
        [rows] { for (QPushButton* b : *rows) b->setChecked(true); });
    QObject::connect(noneBtn, &QPushButton::clicked, noneBtn,
        [rows] { for (QPushButton* b : *rows) b->setChecked(false); });

    tab.page = page;
    tab.populate = [list, empty, rows](const QVector<QPair<qint64, QString>>& items) {
        for (QPushButton* b : *rows) { list->removeWidget(b); b->deleteLater(); }
        rows->clear();
        empty->setVisible(items.isEmpty());
        for (const auto& it : items) {
            QPushButton* b = makeCheckRow(it.second, false);  // unchecked by default
            b->setProperty("cid", QVariant::fromValue(it.first));
            list->insertWidget(list->count() - 1, b);          // before the stretch
            rows->push_back(b);
        }
    };
    return tab;
}

QVector<QPair<qint64, QString>> routeItems(RouteStore* rs) {
    QVector<QPair<qint64, QString>> v;
    for (const Route& r : rs->routes()) {
        QString label = r.name.isEmpty() ? QStringLiteral("(unnamed route)") : r.name;
        label += QStringLiteral("   ·   %1 pt%2")
                     .arg(r.points.size()).arg(r.points.size() == 1 ? "" : "s");
        v.push_back({r.id, label});
    }
    return v;
}

QVector<QPair<qint64, QString>> waypointItems(RouteStore* rs) {
    QVector<QPair<qint64, QString>> v;
    for (const Waypoint& w : rs->waypoints())
        v.push_back({w.id, w.name.isEmpty() ? QStringLiteral("(unnamed waypoint)") : w.name});
    return v;
}

QSet<qint64> checkedIds(const QVector<QPushButton*>& rows) {
    QSet<qint64> ids;
    for (QPushButton* b : rows)
        if (b->isChecked()) ids.insert(b->property("cid").toLongLong());
    return ids;
}

QString documentsDir() {
    const QString d = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return d.isEmpty() ? QDir::homePath() : d;
}

}  // namespace

GpxPlugin::GpxPlugin() = default;
GpxPlugin::~GpxPlugin() = default;

void GpxPlugin::initialize(ICoreApi* core) {
    core_ = core;
    core_->addMenuAction(QStringLiteral("GPX Import / Export…"), [this] { openDialog(); });
}

void GpxPlugin::shutdown() {
    // Nothing persistent registered (the menu item lives for the app's lifetime);
    // the modeless dialog is self-deleting and parented to the core's dialog
    // parent, so it tears down with the main window.
}

void GpxPlugin::openDialog() {
    RouteStore* rs = core_->routes();
    if (!rs) {
        QMessageBox::warning(core_->dialogParent(), QStringLiteral("GPX"),
            QStringLiteral("The routes database is unavailable, so GPX import and "
                           "export cannot run."));
        return;
    }

    auto* dlg = new QDialog(core_->dialogParent());
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QStringLiteral("GPX Import / Export"));
    dlg->resize(480, 660);

    const theme::MenuPalette& t = theme::menu();
    // Modeless and self-deleting (WA_DeleteOnClose), so the ✕ must dismiss via
    // close() rather than reject() — hence closeOnDismiss = true.
    auto* panelCol = dialogchrome::setup(dlg, QStringLiteral("GPX Import / Export"), true);

    auto* body = new QWidget;
    auto* bodyCol = new QVBoxLayout(body);
    bodyCol->setContentsMargins(20, 16, 20, 16);
    bodyCol->setSpacing(12);

    auto* exportHint = new QLabel(QStringLiteral("Select what to export:"));
    exportHint->setStyleSheet(QStringLiteral("font-size:13px; color:%1;")
                                  .arg(theme::textMuted()));
    bodyCol->addWidget(exportHint);

    // ---- Selection tabs (Routes / Waypoints) --------------------------------
    SelTab routeTab = makeSelTab(QStringLiteral("routes"));
    SelTab wptTab   = makeSelTab(QStringLiteral("waypoints"));

    auto* tabs = new QTabWidget;
    tabs->setDocumentMode(true);
    tabs->setStyleSheet(QStringLiteral(
        "QTabWidget::pane{ border:1px solid %2; background:%1; }"
        "QTabBar::tab{ background:%3; color:%4; padding:8px 20px; border:none; font-size:14px; }"
        "QTabBar::tab:selected{ background:%1; color:%5; font-weight:600; }")
        .arg(t.panelBg, t.separator, t.headerBg, t.headerFg, t.actionFg));
    tabs->addTab(routeTab.page, QStringLiteral("Routes"));
    tabs->addTab(wptTab.page,   QStringLiteral("Waypoints"));
    bodyCol->addWidget(tabs, 1);

    auto refreshTabTitles = [tabs, rs] {
        tabs->setTabText(0, QStringLiteral("Routes (%1)").arg(rs->routes().size()));
        tabs->setTabText(1, QStringLiteral("Waypoints (%1)").arg(rs->waypoints().size()));
    };
    auto refreshLists = [=] {
        routeTab.populate(routeItems(rs));
        wptTab.populate(waypointItems(rs));
        refreshTabTitles();
    };
    refreshLists();

    // Result of the last operation; stays visible so a glance confirms success.
    auto* status = new QLabel;
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("font-size:14px; color:%1;").arg(t.actionFg));
    bodyCol->addWidget(status);

    // ---- Import / Export actions, side by side at the bottom ----------------
    auto* importBtn = bigButton(QStringLiteral("Import GPX File…"));
    auto* exportBtn = bigButton(QStringLiteral("Export GPX File…"));
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(10);
    actionRow->addWidget(importBtn, 1);
    actionRow->addWidget(exportBtn, 1);
    bodyCol->addLayout(actionRow);

    panelCol->addWidget(body, 1);

    const QString okColor = t.actionFg;
    const auto showError = [status](const QString& msg) {
        status->setStyleSheet(QStringLiteral("font-size:14px; color:%1;").arg(theme::isDark()
            ? QStringLiteral("#ff8a80") : QStringLiteral("#b00020")));
        status->setText(msg);
    };
    const auto showOk = [status, okColor](const QString& msg) {
        status->setStyleSheet(QStringLiteral("font-size:14px; color:%1;").arg(okColor));
        status->setText(msg);
    };

    // ---- Import -------------------------------------------------------------
    QObject::connect(importBtn, &QPushButton::clicked, dlg, [=] {
        const QString path = QFileDialog::getOpenFileName(
            dlg, QStringLiteral("Import GPX"), documentsDir(),
            QStringLiteral("GPX files (*.gpx);;All files (*)"));
        if (path.isEmpty()) return;

        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            showError(QStringLiteral("Could not open %1.").arg(QFileInfo(path).fileName()));
            return;
        }
        gpx::Document doc;
        QString err;
        if (!gpx::read(f.readAll(), doc, err)) {
            showError(QStringLiteral("Could not read GPX: %1").arg(err));
            return;
        }
        if (doc.routes.isEmpty() && doc.waypoints.isEmpty()) {
            showError(QStringLiteral("No routes or waypoints found in %1.")
                          .arg(QFileInfo(path).fileName()));
            return;
        }
        // Insert into the store; it assigns fresh ids. Give unnamed records a
        // sensible default so they don't show as "(unnamed)" in the lists.
        for (Waypoint w : doc.waypoints) {
            if (w.name.isEmpty()) w.name = rs->nextWaypointName();
            rs->addWaypoint(w);
        }
        for (Route r : doc.routes) {
            if (r.name.isEmpty()) r.name = rs->nextRouteName();
            rs->addRoute(r);
        }
        refreshLists();   // surface the imported records in the export tabs
        showOk(QStringLiteral("Imported %1 route%2 and %3 waypoint%4 from %5.")
                   .arg(doc.routes.size()).arg(doc.routes.size() == 1 ? "" : "s")
                   .arg(doc.waypoints.size()).arg(doc.waypoints.size() == 1 ? "" : "s")
                   .arg(QFileInfo(path).fileName()));
    });

    // ---- Export -------------------------------------------------------------
    QObject::connect(exportBtn, &QPushButton::clicked, dlg, [=] {
        const QSet<qint64> routeSel = checkedIds(*routeTab.rows);
        const QSet<qint64> wptSel   = checkedIds(*wptTab.rows);
        if (routeSel.isEmpty() && wptSel.isEmpty()) {
            showError(QStringLiteral("Select at least one route or waypoint to export."));
            return;
        }

        // Filter the store snapshots down to the ticked ids, preserving order.
        QVector<Route> routes;
        for (const Route& r : rs->routes())
            if (routeSel.contains(r.id)) routes.push_back(r);
        QVector<Waypoint> waypoints;
        for (const Waypoint& w : rs->waypoints())
            if (wptSel.contains(w.id)) waypoints.push_back(w);

        const QString suggested = documentsDir() + QStringLiteral("/chartplotter-%1.gpx")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd")));
        QString path = QFileDialog::getSaveFileName(
            dlg, QStringLiteral("Export GPX"), suggested,
            QStringLiteral("GPX files (*.gpx);;All files (*)"));
        if (path.isEmpty()) return;
        if (!path.endsWith(QStringLiteral(".gpx"), Qt::CaseInsensitive))
            path += QStringLiteral(".gpx");

        const QByteArray data = gpx::write(routes, waypoints);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            showError(QStringLiteral("Could not write %1.").arg(QFileInfo(path).fileName()));
            return;
        }
        f.write(data);
        f.close();
        showOk(QStringLiteral("Exported %1 route%2 and %3 waypoint%4 to %5.")
                   .arg(routes.size()).arg(routes.size() == 1 ? "" : "s")
                   .arg(waypoints.size()).arg(waypoints.size() == 1 ? "" : "s")
                   .arg(QFileInfo(path).fileName()));
    });

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}
