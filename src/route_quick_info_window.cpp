#include "route_quick_info_window.hpp"
#include "route_store.hpp"
#include "theme.hpp"
#include "units.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>

RouteQuickInfoWindow::RouteQuickInfoWindow(ClickedRouteObject::Kind kind, qint64 id,
                                           const RouteStore* store, QWidget* parent)
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint),
      kind_(kind), id_(id), store_(store) {
    // Show without grabbing focus so chart pans still dismiss this popup, and
    // delete on close so the host's QPointer clears.
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setFrameShape(QFrame::StyledPanel);
    setStyleSheet(QStringLiteral(
        "RouteQuickInfoWindow { background:%1; border:1px solid %2; border-radius:6px; }"
        "QPushButton { min-height:34px; padding:0 12px; }")
        .arg(theme::menu().panelBg, theme::menu().panelBorder));

    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(12, 8, 12, 10);
    col->setSpacing(4);

    // Title row: name on the left, a close (X) button pinned to the top-right —
    // the popup's explicit dismiss (chart interaction also closes it).
    auto* titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(8);
    titleLabel_ = new QLabel(this);
    titleLabel_->setStyleSheet(QStringLiteral("font-size:15px; font-weight:600;"));
    titleRow->addWidget(titleLabel_, 1);

    auto* closeBtn = new QPushButton(QString(QChar(0x2715)), this);   // U+2715 MULTIPLICATION X
    closeBtn->setFixedSize(26, 26);
    closeBtn->setCursor(Qt::PointingHandCursor);
    // Override this window's global QPushButton rule (min-height/padding) so the X
    // stays compact in the corner.
    closeBtn->setStyleSheet(QStringLiteral(
        "QPushButton{ min-height:0; padding:0; border:none; background:transparent;"
        " color:%1; font-size:15px; font-weight:600; }"
        "QPushButton:pressed{ color:%2; }")
        .arg(theme::textMuted(), theme::menu().actionFg));
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);
    titleRow->addWidget(closeBtn, 0, Qt::AlignTop);
    col->addLayout(titleRow);

    subLabel_ = new QLabel(this);
    subLabel_->setStyleSheet(QStringLiteral("font-size:13px;"));
    col->addWidget(subLabel_);

    // "Navigate" is the headline action for a route, so it gets its own full-width
    // accented button above the edit/manage grid. Waypoints can't be navigated.
    if (kind_ == ClickedRouteObject::Kind::Route) {
        auto* navBtn = new QPushButton(QStringLiteral("Navigate"), this);
        navBtn->setStyleSheet(QStringLiteral(
            "QPushButton{ min-height:34px; font-weight:600; color:white;"
            " background:%1; border-radius:4px; }").arg(theme::menu().accent));
        col->addWidget(navBtn);
        connect(navBtn, &QPushButton::clicked, this, [this] {
            emit navigateRequested();
            close();
        });
    }

    // Action buttons. Two rows of two so the popup stays narrow on a tablet.
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(6);
    grid->setContentsMargins(0, 6, 0, 0);

    auto* renameBtn = new QPushButton(QStringLiteral("Rename"), this);
    auto* editBtn   = new QPushButton(
        kind_ == ClickedRouteObject::Kind::Route
            ? QStringLiteral("Edit on chart")
            : QStringLiteral("Drag on chart"), this);
    auto* propsBtn  = new QPushButton(QStringLiteral("Properties…"), this);
    visBtn_         = new QPushButton(QStringLiteral("Hide"), this);
    auto* delBtn    = new QPushButton(QStringLiteral("Delete"), this);

    grid->addWidget(renameBtn, 0, 0);
    grid->addWidget(editBtn,   0, 1);
    grid->addWidget(propsBtn,  1, 0);
    grid->addWidget(visBtn_,   1, 1);
    grid->addWidget(delBtn,    2, 0, 1, 2);
    col->addLayout(grid);

    connect(renameBtn, &QPushButton::clicked, this, [this] { emit renameRequested(); close(); });
    connect(editBtn,   &QPushButton::clicked, this, [this] { emit editRequested(); close(); });
    connect(propsBtn,  &QPushButton::clicked, this, [this] { emit propertiesRequested(); close(); });
    connect(visBtn_,   &QPushButton::clicked, this, [this] {
        emit visibilityToggleRequested();
        close();
    });
    connect(delBtn,    &QPushButton::clicked, this, [this] { emit deleteRequested(); close(); });

    if (store_) {
        connect(store_, &RouteStore::routesChanged,    this, &RouteQuickInfoWindow::refresh);
        connect(store_, &RouteStore::waypointsChanged, this, &RouteQuickInfoWindow::refresh);
    }
    refresh();
}

void RouteQuickInfoWindow::refresh() {
    if (!store_) return;
    if (kind_ == ClickedRouteObject::Kind::Waypoint) {
        const Waypoint* w = nullptr;
        for (const Waypoint& cand : store_->waypoints())
            if (cand.id == id_) { w = &cand; break; }
        if (!w) { close(); return; }   // deleted out from under us
        titleLabel_->setText(w->name.isEmpty() ? QStringLiteral("(unnamed waypoint)") : w->name);
        subLabel_->setText(units::formatLatitude(w->lat) + QStringLiteral("  ")
                           + units::formatLongitude(w->lon));
        visBtn_->setText(w->visible ? QStringLiteral("Hide") : QStringLiteral("Show"));
    } else {
        const Route* r = store_->route(id_);
        if (!r) { close(); return; }
        titleLabel_->setText(r->name.isEmpty() ? QStringLiteral("(unnamed route)") : r->name);
        subLabel_->setText(QStringLiteral("%1 point%2")
                            .arg(r->points.size())
                            .arg(r->points.size() == 1 ? "" : "s"));
        visBtn_->setText(r->visible ? QStringLiteral("Hide") : QStringLiteral("Show"));
    }
    adjustSize();
}
