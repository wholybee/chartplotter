#include "route_properties_dialog.hpp"
#include "units.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScroller>
#include <QFrame>
#include <QWidget>
#include <QMessageBox>

namespace {
// Coordinate entry field, shown in the user's selected lat/lon format. Free text
// (no validator) since formats like DMS aren't plain numbers; the input is parsed
// tolerantly on commit via units::parseLatitude/Longitude.
QLineEdit* makeCoordEdit(double value, bool isLat) {
    const theme::MenuPalette& t = theme::menu();
    const theme::InputPalette& in = theme::input();
    auto* e = new QLineEdit;
    e->setMinimumHeight(36);
    e->setText(isLat ? units::formatLatitude(value) : units::formatLongitude(value));
    e->setStyleSheet(QStringLiteral(
        "QLineEdit{ font-size:14px; padding:4px 8px; background:%1; color:%2;"
        " border:1px solid %3; border-radius:6px; }"
        "QLineEdit:focus{ border:1px solid %4; }")
        .arg(in.fieldBg, in.fg, in.border, t.accent));
    return e;
}
}  // namespace

RoutePropertiesDialog::RoutePropertiesDialog(const Route& route, QWidget* parent)
    : QDialog(parent), work_(route) {
    setWindowTitle(QStringLiteral("Route Properties"));
    resize(520, 620);

    const theme::MenuPalette& t = theme::menu();
    // Modeless + self-deleting (WA_DeleteOnClose), single-instance; dismiss via
    // close() so the dialog is destroyed and the owner's pointer clears.
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Route Properties"), true);
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Details")));

    auto* body = new QWidget;
    body->setStyleSheet(QStringLiteral("QLabel{ color:%1; font-size:14px; }").arg(t.actionFg));
    auto* bcol = new QVBoxLayout(body);
    bcol->setContentsMargins(16, 4, 16, 8);
    bcol->setSpacing(8);
    auto* form = new QFormLayout;
    form->setSpacing(8);
    nameEdit_ = new QLineEdit(work_.name);
    dialogchrome::styleLineEdit(nameEdit_);
    descEdit_ = new QLineEdit(work_.description);
    dialogchrome::styleLineEdit(descEdit_);
    form->addRow(QStringLiteral("Name"), nameEdit_);
    form->addRow(QStringLiteral("Description"), descEdit_);
    bcol->addLayout(form);
    panelCol->addWidget(body);

    // Points header doubles as the section-header strip; rebuildRows updates the count.
    countLabel_ = new QLabel;
    countLabel_->setStyleSheet(QStringLiteral(
        "font-size:12px; font-weight:600; color:%1;"
        "padding:14px 16px 6px 16px; background:%2;").arg(t.headerFg, t.headerBg));
    panelCol->addWidget(countLabel_);

    scrollArea_ = new QScrollArea;
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setStyleSheet(QStringLiteral(
        "QScrollArea, QScrollArea > QWidget > QWidget { background:%1; }").arg(t.panelBg));
    rowContainer_ = new QWidget;
    rowLayout_ = new QVBoxLayout(rowContainer_);
    rowLayout_->setContentsMargins(0, 0, 0, 0);
    rowLayout_->setSpacing(0);
    rowLayout_->addStretch(1);
    scrollArea_->setWidget(rowContainer_);
    // Drag-to-scroll, consistent with the list dialogs. Taps still reach the
    // per-row fields and buttons.
    QScroller::grabGesture(scrollArea_->viewport(), QScroller::LeftMouseButtonGesture);
    panelCol->addWidget(scrollArea_, 1);

    auto* btnBar = new QWidget;
    auto* btnRow = new QHBoxLayout(btnBar);
    btnRow->setContentsMargins(16, 8, 16, 16);
    btnRow->setSpacing(10);
    btnRow->addStretch(1);
    auto* cancelBtn = new QPushButton(QStringLiteral("Cancel"));
    auto* okBtn     = new QPushButton(QStringLiteral("OK"));
    dialogchrome::styleOutlinedButton(cancelBtn);
    dialogchrome::styleAccentButton(okBtn);
    okBtn->setDefault(true);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(okBtn);
    panelCol->addWidget(btnBar);

    // Modeless + self-deleting: Cancel closes (destroys) rather than just hiding.
    connect(cancelBtn, &QPushButton::clicked, this, &QWidget::close);
    connect(okBtn,     &QPushButton::clicked, this, &RoutePropertiesDialog::onOk);

    rebuildRows();
}

void RoutePropertiesDialog::rebuildRows() {
    // Tear down existing rows (cheap: point counts are modest, not on a timer).
    for (Row& r : rows_) r.widget->deleteLater();
    rows_.clear();

    const theme::MenuPalette& t = theme::menu();
    countLabel_->setText(QStringLiteral("Points (%1)").arg(work_.points.size()));

    for (int i = 0; i < work_.points.size(); ++i) {
        const RoutePoint& p = work_.points[i];
        auto* w = new QWidget(rowContainer_);
        w->setObjectName(QStringLiteral("RoutePtRow"));
        // Scope the divider to the row (#id) so it doesn't cascade to children.
        w->setStyleSheet(QStringLiteral("#RoutePtRow{ border-bottom:1px solid %1; }")
                             .arg(t.separator));
        auto* hl = new QHBoxLayout(w);
        hl->setContentsMargins(8, 4, 8, 4);
        hl->setSpacing(6);

        auto* num = new QLabel(QString::number(i + 1), w);
        num->setFixedWidth(28);
        num->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        num->setStyleSheet(QStringLiteral("border:none; font-weight:600; color:%1;")
                               .arg(t.actionFg));
        hl->addWidget(num);

        Row row;
        row.widget = w;
        row.lat = makeCoordEdit(p.lat, /*isLat=*/true);
        row.lon = makeCoordEdit(p.lon, /*isLat=*/false);
        row.lat->setToolTip(QStringLiteral("Latitude"));
        row.lon->setToolTip(QStringLiteral("Longitude"));
        hl->addWidget(row.lat, 1);
        hl->addWidget(row.lon, 1);

        auto* editBtn = new QPushButton(QStringLiteral("Edit"), w);
        dialogchrome::styleOutlinedButton(editBtn);
        editBtn->setToolTip(QStringLiteral("Drag this point on the chart"));
        connect(editBtn, &QPushButton::clicked, this, [this, i] { emit editPointRequested(i); });
        hl->addWidget(editBtn);

        auto* delBtn = new QPushButton(QStringLiteral("Delete"), w);
        dialogchrome::styleOutlinedButton(delBtn);
        connect(delBtn, &QPushButton::clicked, this, [this, i] { onDeletePoint(i); });
        hl->addWidget(delBtn);

        rowLayout_->insertWidget(rowLayout_->count() - 1, w);   // before the stretch
        rows_.push_back(row);
    }
}

void RoutePropertiesDialog::commitFieldsToWorking() {
    work_.name = nameEdit_->text().trimmed();
    work_.description = descEdit_->text().trimmed();
    for (int i = 0; i < int(rows_.size()) && i < work_.points.size(); ++i) {
        bool ok = false;
        const double la = units::parseLatitude(rows_[i].lat->text(), &ok);
        if (ok) work_.points[i].lat = la;             // keep old value if unparseable
        const double lo = units::parseLongitude(rows_[i].lon->text(), &ok);
        if (ok) work_.points[i].lon = lo;
    }
}

void RoutePropertiesDialog::onDeletePoint(int index) {
    if (index < 0 || index >= work_.points.size()) return;
    commitFieldsToWorking();          // preserve other edits before rebuilding
    work_.points.remove(index);
    rebuildRows();
}

Route RoutePropertiesDialog::currentRoute() const {
    Route r = work_;                  // keeps id / created / visible
    r.name = nameEdit_->text().trimmed();
    r.description = descEdit_->text().trimmed();
    for (int i = 0; i < int(rows_.size()) && i < r.points.size(); ++i) {
        bool ok = false;
        const double la = units::parseLatitude(rows_[i].lat->text(), &ok);
        if (ok) r.points[i].lat = la;                 // fall back to stored value
        const double lo = units::parseLongitude(rows_[i].lon->text(), &ok);
        if (ok) r.points[i].lon = lo;
    }
    return r;
}

void RoutePropertiesDialog::setRoute(const Route& route) {
    work_ = route;
    nameEdit_->setText(work_.name);
    descEdit_->setText(work_.description);
    rebuildRows();
}

void RoutePropertiesDialog::onOk() {
    // Every coordinate field must parse (and be in range) in the current format.
    for (int i = 0; i < int(rows_.size()); ++i) {
        bool okLat = false, okLon = false;
        units::parseLatitude(rows_[i].lat->text(), &okLat);
        units::parseLongitude(rows_[i].lon->text(), &okLon);
        if (!okLat || !okLon) {
            QMessageBox::information(this, QStringLiteral("Route Properties"),
                QStringLiteral("Point %1 has an invalid latitude/longitude.").arg(i + 1));
            return;
        }
    }
    commitFieldsToWorking();
    if (work_.points.size() < 2) {
        QMessageBox::information(this, QStringLiteral("Route Properties"),
            QStringLiteral("A route needs at least two points."));
        return;
    }
    accept();
}
