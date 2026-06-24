#include "waypoint_properties_dialog.hpp"
#include "units.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>
#include <QMessageBox>

WaypointPropertiesDialog::WaypointPropertiesDialog(const Waypoint& wpt, QWidget* parent)
    : QDialog(parent), work_(wpt) {
    setWindowTitle(QStringLiteral("Waypoint Properties"));
    resize(440, 380);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Waypoint Properties"));
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Details")));

    auto* body = new QWidget;
    body->setStyleSheet(QStringLiteral("QLabel{ color:%1; font-size:14px; }").arg(t.actionFg));
    auto* bcol = new QVBoxLayout(body);
    bcol->setContentsMargins(16, 4, 16, 12);
    bcol->setSpacing(8);

    auto* form = new QFormLayout;
    form->setSpacing(8);
    nameEdit_ = new QLineEdit(work_.name);
    dialogchrome::styleLineEdit(nameEdit_);
    descEdit_ = new QLineEdit(work_.description);
    dialogchrome::styleLineEdit(descEdit_);
    // Free text (no validator): coordinate formats like DMS aren't plain numbers;
    // parsed tolerantly on OK via units::parseLatitude/Longitude.
    latEdit_ = new QLineEdit(units::formatLatitude(work_.lat));
    dialogchrome::styleLineEdit(latEdit_);
    lonEdit_ = new QLineEdit(units::formatLongitude(work_.lon));
    dialogchrome::styleLineEdit(lonEdit_);
    form->addRow(QStringLiteral("Name"), nameEdit_);
    form->addRow(QStringLiteral("Description"), descEdit_);
    form->addRow(QStringLiteral("Latitude"), latEdit_);
    form->addRow(QStringLiteral("Longitude"), lonEdit_);
    bcol->addLayout(form);
    panelCol->addWidget(body);

    panelCol->addStretch(1);

    auto* btnBar = new QWidget;
    auto* row = new QHBoxLayout(btnBar);
    row->setContentsMargins(16, 8, 16, 16);
    row->setSpacing(10);
    auto* cancelBtn = new QPushButton(QStringLiteral("Cancel"));
    auto* okBtn     = new QPushButton(QStringLiteral("OK"));
    dialogchrome::styleOutlinedButton(cancelBtn);
    dialogchrome::styleAccentButton(okBtn);
    okBtn->setDefault(true);
    row->addStretch(1);
    row->addWidget(cancelBtn);
    row->addWidget(okBtn);
    panelCol->addWidget(btnBar);

    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(okBtn,     &QPushButton::clicked, this, &WaypointPropertiesDialog::onOk);

    nameEdit_->setFocus();
    nameEdit_->selectAll();
}

Waypoint WaypointPropertiesDialog::currentWaypoint() const {
    Waypoint w = work_;                 // keeps id / created / symbol / visible
    w.name = nameEdit_->text().trimmed();
    w.description = descEdit_->text().trimmed();
    bool ok = false;
    const double la = units::parseLatitude(latEdit_->text(), &ok);
    if (ok) w.lat = la;
    const double lo = units::parseLongitude(lonEdit_->text(), &ok);
    if (ok) w.lon = lo;
    return w;
}

void WaypointPropertiesDialog::onOk() {
    bool okLat = false, okLon = false;
    units::parseLatitude(latEdit_->text(), &okLat);
    units::parseLongitude(lonEdit_->text(), &okLon);
    if (!okLat || !okLon) {
        QMessageBox::information(this, QStringLiteral("Waypoint Properties"),
            QStringLiteral("Please enter a valid latitude and longitude."));
        return;
    }
    accept();
}
