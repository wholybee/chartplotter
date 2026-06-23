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
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDateTime>

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

QString summaryText(RouteStore* rs) {
    const int nr = rs->routes().size();
    const int nw = rs->waypoints().size();
    return QStringLiteral("Database: %1 route%2, %3 waypoint%4.")
        .arg(nr).arg(nr == 1 ? "" : "s")
        .arg(nw).arg(nw == 1 ? "" : "s");
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
    dlg->resize(460, 460);

    const theme::MenuPalette& t = theme::menu();
    // Modeless and self-deleting (WA_DeleteOnClose), so the ✕ must dismiss via
    // close() rather than reject() — hence closeOnDismiss = true.
    auto* panelCol = dialogchrome::setup(dlg, QStringLiteral("GPX Import / Export"), true);

    auto* body = new QWidget;
    auto* bodyCol = new QVBoxLayout(body);
    bodyCol->setContentsMargins(20, 16, 20, 16);
    bodyCol->setSpacing(14);

    auto* intro = new QLabel(QStringLiteral(
        "Import routes and waypoints from a GPX file into the chartplotter, or "
        "export everything currently stored to a GPX file you can share or back up."));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("font-size:15px; color:%1;").arg(t.actionFg));
    bodyCol->addWidget(intro);

    auto* summary = new QLabel(summaryText(rs));
    summary->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(theme::textMuted()));
    bodyCol->addWidget(summary);

    auto* importBtn = bigButton(QStringLiteral("Import GPX File…"));
    auto* exportBtn = bigButton(QStringLiteral("Export GPX File…"));
    bodyCol->addWidget(importBtn);
    bodyCol->addWidget(exportBtn);

    bodyCol->addStretch(1);

    // Result of the last operation; stays visible so a glance confirms success.
    auto* status = new QLabel;
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("font-size:14px; color:%1;").arg(t.actionFg));
    bodyCol->addWidget(status);

    auto* footer = new QHBoxLayout;
    footer->addStretch(1);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"));
    dialogchrome::styleOutlinedButton(closeBtn);
    QObject::connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::close);
    footer->addWidget(closeBtn);
    bodyCol->addLayout(footer);

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
        summary->setText(summaryText(rs));
        showOk(QStringLiteral("Imported %1 route%2 and %3 waypoint%4 from %5.")
                   .arg(doc.routes.size()).arg(doc.routes.size() == 1 ? "" : "s")
                   .arg(doc.waypoints.size()).arg(doc.waypoints.size() == 1 ? "" : "s")
                   .arg(QFileInfo(path).fileName()));
    });

    // ---- Export -------------------------------------------------------------
    QObject::connect(exportBtn, &QPushButton::clicked, dlg, [=] {
        if (rs->routes().isEmpty() && rs->waypoints().isEmpty()) {
            showError(QStringLiteral("There are no routes or waypoints to export."));
            return;
        }
        const QString suggested = documentsDir() + QStringLiteral("/chartplotter-%1.gpx")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd")));
        QString path = QFileDialog::getSaveFileName(
            dlg, QStringLiteral("Export GPX"), suggested,
            QStringLiteral("GPX files (*.gpx);;All files (*)"));
        if (path.isEmpty()) return;
        if (!path.endsWith(QStringLiteral(".gpx"), Qt::CaseInsensitive))
            path += QStringLiteral(".gpx");

        const QByteArray data = gpx::write(rs->routes(), rs->waypoints());
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            showError(QStringLiteral("Could not write %1.").arg(QFileInfo(path).fileName()));
            return;
        }
        f.write(data);
        f.close();
        showOk(QStringLiteral("Exported %1 route%2 and %3 waypoint%4 to %5.")
                   .arg(rs->routes().size()).arg(rs->routes().size() == 1 ? "" : "s")
                   .arg(rs->waypoints().size()).arg(rs->waypoints().size() == 1 ? "" : "s")
                   .arg(QFileInfo(path).fileName()));
    });

    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}
