#include "units_dialog.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QRadioButton>
#include <QButtonGroup>
#include <QWidget>

namespace {
// A radio button tall enough for touch, carrying its enum value as the button id.
QRadioButton* addChoice(QButtonGroup* group, QVBoxLayout* into,
                        const QString& text, int id, bool checked) {
    auto* rb = new QRadioButton(text);
    rb->setMinimumHeight(40);
    rb->setCursor(Qt::PointingHandCursor);
    rb->setStyleSheet(QStringLiteral("QRadioButton{ font-size:15px; color:%1; spacing:8px; }")
                      .arg(theme::menu().actionFg));
    rb->setChecked(checked);
    group->addButton(rb, id);
    into->addWidget(rb);
    return rb;
}
} // namespace

UnitsDialog::UnitsDialog(DepthUnit depth, DistanceUnit distance, AngleFormat angle,
                         BearingMode bearing, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Units"));
    resize(440, 660);

    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Units"));

    // Each unit group: a full-width section header plus a margined body of radios.
    auto group = [&](const QString& title) {
        panelCol->addWidget(dialogchrome::sectionHeader(title));
        auto* w = new QWidget;
        auto* l = new QVBoxLayout(w);
        l->setContentsMargins(16, 4, 16, 8);
        l->setSpacing(4);
        panelCol->addWidget(w);
        return l;
    };

    // ---- Depth ----
    auto* depthCol = group(QStringLiteral("Depth (Soundings)"));
    depthGroup_ = new QButtonGroup(this);
    addChoice(depthGroup_, depthCol, units::depthUnitLabel(DepthUnit::Feet),
              int(DepthUnit::Feet),   depth == DepthUnit::Feet);
    addChoice(depthGroup_, depthCol, units::depthUnitLabel(DepthUnit::Meters),
              int(DepthUnit::Meters), depth == DepthUnit::Meters);

    // ---- Distance ----
    auto* distCol = group(QStringLiteral("Distance"));
    distGroup_ = new QButtonGroup(this);
    addChoice(distGroup_, distCol, units::distanceUnitLabel(DistanceUnit::NauticalMiles),
              int(DistanceUnit::NauticalMiles), distance == DistanceUnit::NauticalMiles);
    addChoice(distGroup_, distCol, units::distanceUnitLabel(DistanceUnit::StatuteMiles),
              int(DistanceUnit::StatuteMiles),  distance == DistanceUnit::StatuteMiles);
    addChoice(distGroup_, distCol, units::distanceUnitLabel(DistanceUnit::Kilometers),
              int(DistanceUnit::Kilometers),    distance == DistanceUnit::Kilometers);

    // ---- Coordinates (lat/lon display) ----
    auto* angleCol = group(QStringLiteral("Coordinates"));
    angleGroup_ = new QButtonGroup(this);
    addChoice(angleGroup_, angleCol, units::angleFormatLabel(AngleFormat::DecimalDegrees),
              int(AngleFormat::DecimalDegrees), angle == AngleFormat::DecimalDegrees);
    addChoice(angleGroup_, angleCol, units::angleFormatLabel(AngleFormat::DegMinutes),
              int(AngleFormat::DegMinutes),     angle == AngleFormat::DegMinutes);
    addChoice(angleGroup_, angleCol, units::angleFormatLabel(AngleFormat::DegMinSec),
              int(AngleFormat::DegMinSec),      angle == AngleFormat::DegMinSec);

    // ---- Bearings (true vs magnetic) ----
    auto* bearingCol = group(QStringLiteral("Bearings"));
    bearingGroup_ = new QButtonGroup(this);
    addChoice(bearingGroup_, bearingCol, units::bearingModeLabel(BearingMode::True),
              int(BearingMode::True),     bearing == BearingMode::True);
    addChoice(bearingGroup_, bearingCol, units::bearingModeLabel(BearingMode::Magnetic),
              int(BearingMode::Magnetic), bearing == BearingMode::Magnetic);

    panelCol->addStretch(1);
    panelCol->addWidget(dialogchrome::okCancelRow(this));
}

DepthUnit UnitsDialog::depthUnit() const {
    return DepthUnit(depthGroup_->checkedId());
}

DistanceUnit UnitsDialog::distanceUnit() const {
    return DistanceUnit(distGroup_->checkedId());
}

AngleFormat UnitsDialog::angleFormat() const {
    return AngleFormat(angleGroup_->checkedId());
}

BearingMode UnitsDialog::bearingMode() const {
    return BearingMode(bearingGroup_->checkedId());
}
