#include "nav_data_browser_window.hpp"
#include "nav_data_store.hpp"
#include "theme.hpp"
#include "units.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QLabel>
#include <QWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QTimer>
#include <QColor>
#include <QVariant>
#include <QDateTime>
#include <functional>
#include <vector>

NavDataBrowserWindow::NavDataBrowserWindow(const NavDataStore* store, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle(QStringLiteral("NavData Browser"));
    resize(560, 660);

    const theme::MenuPalette& t = theme::menu();
    // Modeless, reused window (kept by the owner); the ✕ closes/hides it.
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("NavData Browser"), true);

    const QString tableStyle = QStringLiteral(
        "QTableWidget{ background:%1; color:%2; gridline-color:%3; border:none; }"
        "QHeaderView::section{ background:%4; color:%5; border:none; padding:6px;"
        " font-size:12px; font-weight:600; }"
        "QTableCornerButton::section{ background:%4; border:none; }")
        .arg(t.panelBg, t.actionFg, t.separator, t.headerBg, t.headerFg);

    auto* body = new QWidget;
    auto* col = new QVBoxLayout(body);
    col->setContentsMargins(12, 8, 12, 8);
    col->setSpacing(8);

    table_ = new QTableWidget(0, 4);
    table_->setHorizontalHeaderLabels(
        {QStringLiteral("Field"), QStringLiteral("Value"),
         QStringLiteral("Source"), QStringLiteral("Age")});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setFocusPolicy(Qt::NoFocus);
    table_->setStyleSheet(tableStyle);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    col->addWidget(table_, 3);

    // Route navigation (APB/RMB) section: computed values, so just Field / Value.
    auto* navHeader = new QLabel(QStringLiteral("Navigation (APB / RMB)"));
    navHeader->setStyleSheet(QStringLiteral(
        "font-size:13px; font-weight:600; color:%1; padding:4px 2px;").arg(t.headerFg));
    col->addWidget(navHeader);

    navTable_ = new QTableWidget(0, 2);
    navTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Field"), QStringLiteral("Value")});
    navTable_->verticalHeader()->setVisible(false);
    navTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    navTable_->setSelectionMode(QAbstractItemView::NoSelection);
    navTable_->setFocusPolicy(Qt::NoFocus);
    navTable_->setStyleSheet(tableStyle);
    navTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    navTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    col->addWidget(navTable_, 2);
    panelCol->addWidget(body, 1);

    if (store_) {
        connect(store_, &NavDataStore::ownshipChanged, this, &NavDataBrowserWindow::refresh);
        connect(store_, &NavDataStore::navigationChanged,
                this, &NavDataBrowserWindow::refreshNavigation);
    }
    // Tick so Age keeps counting even when no new data or transition arrives.
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &NavDataBrowserWindow::refresh);
    timer_->start();

    refresh();
}

void NavDataBrowserWindow::setCell(QTableWidget* table, int row, int col,
                                   const QString& text, bool muted) {
    QTableWidgetItem* it = table->item(row, col);
    if (!it) {
        it = new QTableWidgetItem;
        table->setItem(row, col, it);
    }
    it->setText(text);
    // Muted (stale): pin a theme-aware grey. Fresh: clear any prior brush so
    // the cell uses Qt's palette text colour (theme-correct light or dark).
    if (muted) it->setForeground(QColor(theme::textMuted()));
    else       it->setData(Qt::ForegroundRole, QVariant());
}

void NavDataBrowserWindow::refresh() {
    if (!store_) return;
    const OwnshipState& s = store_->ownship();
    const QDateTime now = QDateTime::currentDateTimeUtc();

    struct Row { QString name, value, source, age; bool stale; };
    std::vector<Row> rows;

    auto add = [&](const QString& name, const NavValue& v,
                   const std::function<QString(double)>& fmt) {
        if (!v.valid()) return;   // never set or aged to Invalid -> not listed
        const double age = v.timestampUtc.isValid()
                             ? v.timestampUtc.msecsTo(now) / 1000.0 : 0.0;
        rows.push_back({ name, fmt(v.value), v.source,
                         QString::number(age, 'f', 1) + QStringLiteral(" s"),
                         v.stale() });
    };
    auto angle = [](double v) { return QString::number(v, 'f', 1) + QStringLiteral("°"); };
    auto latFmt = [](double v) { return units::formatLatitude(v); };
    auto lonFmt = [](double v) { return units::formatLongitude(v); };
    auto knots = [](double v) { return QString::number(v, 'f', 1) + QStringLiteral(" kn"); };
    auto metres = [](double v) { return QString::number(v, 'f', 1) + QStringLiteral(" m"); };

    add(QStringLiteral("Latitude"),         s.latitudeDeg,            latFmt);
    add(QStringLiteral("Longitude"),        s.longitudeDeg,           lonFmt);
    add(QStringLiteral("COG (true)"),       s.cogDegTrue,             angle);
    add(QStringLiteral("SOG"),              s.sogKnots,               knots);
    add(QStringLiteral("Water speed"),      s.waterSpeedKnots,        knots);
    add(QStringLiteral("Heading (true)"),   s.headingDegTrue,         angle);
    add(QStringLiteral("Heading (mag)"),    s.headingDegMag,          angle);
    add(QStringLiteral("Variation"),        s.variationDeg,           angle);
    add(QStringLiteral("Depth"),            s.depthMeters,            metres);
    add(QStringLiteral("App. wind angle"),  s.apparentWindAngleDeg,   angle);
    add(QStringLiteral("App. wind speed"),  s.apparentWindSpeedKnots, knots);
    add(QStringLiteral("True wind angle"),  s.trueWindAngleDeg,       angle);
    add(QStringLiteral("True wind speed"),  s.trueWindSpeedKnots,     knots);
    add(QStringLiteral("True wind dir"),    s.trueWindDirectionDeg,   angle);

    // Update in place (resize only when the field set changes) to avoid flicker.
    if (table_->rowCount() != int(rows.size()))
        table_->setRowCount(int(rows.size()));
    for (int i = 0; i < int(rows.size()); ++i) {
        const bool muted = rows[i].stale;
        setCell(table_, i, 0, rows[i].name,   muted);
        setCell(table_, i, 1, rows[i].value,  muted);
        setCell(table_, i, 2, rows[i].source, muted);
        setCell(table_, i, 3, rows[i].age,    muted);
    }

    refreshNavigation();
}

void NavDataBrowserWindow::refreshNavigation() {
    if (!store_ || !navTable_) return;
    const NavigationData& n = store_->navigation();

    struct Row { QString name, value; };
    std::vector<Row> rows;

    if (!n.active) {
        // Nothing to follow: a single status row keeps the section self-explaining.
        rows.push_back({ QStringLiteral("Status"), QStringLiteral("Not navigating") });
    } else {
        auto yesNo   = [](bool b) { return b ? QStringLiteral("Yes") : QStringLiteral("No"); };
        auto deg1    = [](double v) { return QString::number(v, 'f', 1) + QStringLiteral("°"); };
        auto bearing = [&](double v, QChar u) { return deg1(v) + QLatin1Char(' ') + u; };
        auto nm3     = [](double v) { return QString::number(v, 'f', 3) + QStringLiteral(" nm"); };
        auto knots   = [](double v) { return QString::number(v, 'f', 1) + QStringLiteral(" kn"); };

        rows.push_back({ QStringLiteral("Status"), QStringLiteral("Navigating") });
        rows.push_back({ QStringLiteral("XTE magnitude"),          nm3(n.xteNm) });
        rows.push_back({ QStringLiteral("Direction to steer"),     QString(n.steerDirection) });
        rows.push_back({ QStringLiteral("Cross-track units"),      QString(n.xteUnits) });
        rows.push_back({ QStringLiteral("Arrival circle entered"), yesNo(n.arrivalCircleEntered) });
        rows.push_back({ QStringLiteral("Perpendicular passed"),   yesNo(n.perpendicularPassed) });
        rows.push_back({ QStringLiteral("Bearing origin→dest"),
                         bearing(n.bearingOriginToDestDeg, n.bearingUnits) });
        rows.push_back({ QStringLiteral("Bearing units"),          QString(n.bearingUnits) });
        rows.push_back({ QStringLiteral("Destination waypoint"),   n.destinationWaypointId });
        rows.push_back({ QStringLiteral("Bearing present→dest"),
                         bearing(n.bearingPresentToDestDeg, n.bearingUnits) });
        rows.push_back({ QStringLiteral("Heading to steer"),
                         bearing(n.headingToSteerDeg, n.bearingUnits) });
        rows.push_back({ QStringLiteral("Origin waypoint"),
                         n.originWaypointId.isEmpty() ? QStringLiteral("—") : n.originWaypointId });
        rows.push_back({ QStringLiteral("Destination latitude"),
                         units::formatLatitude(n.destinationLatDeg) });
        rows.push_back({ QStringLiteral("Destination longitude"),
                         units::formatLongitude(n.destinationLonDeg) });
        rows.push_back({ QStringLiteral("Range to destination"),   nm3(n.rangeToDestNm) });
        rows.push_back({ QStringLiteral("Closing velocity (VMG)"), knots(n.closingVelocityKn) });
        rows.push_back({ QStringLiteral("FAA status"),             QString(n.faaStatus) });
        rows.push_back({ QStringLiteral("FAA mode"),               QString(n.faaMode) });
    }

    if (navTable_->rowCount() != int(rows.size()))
        navTable_->setRowCount(int(rows.size()));
    for (int i = 0; i < int(rows.size()); ++i) {
        setCell(navTable_, i, 0, rows[i].name,  false);
        setCell(navTable_, i, 1, rows[i].value, false);
    }
}
