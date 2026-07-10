#include "tracking_dialog.hpp"
#include "touch_spin_box.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QLabel>
#include <QWidget>

TrackingDialog::TrackingDialog(double intervalSeconds, double minDistanceNm,
                               DepthUnit shortUnit, QWidget* parent)
    : QDialog(parent), shortUnit_(shortUnit) {
    setWindowTitle(QStringLiteral("Tracking"));
    resize(440, 420);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Tracking"));
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Track Point Interval")));

    auto* body = new QWidget;
    auto* col = new QVBoxLayout(body);
    col->setContentsMargins(16, 4, 16, 12);
    col->setSpacing(10);

    auto* intro = new QLabel(QStringLiteral(
        "While tracking is on, a track point is recorded once this much time has "
        "passed AND the boat has moved at least this far. The distance keeps a boat "
        "at anchor from piling up points in one spot."));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(t.actionFg));
    col->addWidget(intro);

    auto* timeLbl = new QLabel(QStringLiteral("Time"));
    timeLbl->setStyleSheet(QStringLiteral("font-size:14px; color:%1;").arg(t.actionFg));
    col->addWidget(timeLbl);
    secondsBox_ = new TouchSpinBox;
    secondsBox_->setRange(1.0, 3600.0);
    secondsBox_->setSingleStep(5.0);
    secondsBox_->setDecimals(0);
    secondsBox_->setSuffix(QStringLiteral(" s"));
    secondsBox_->setValue(intervalSeconds);
    col->addWidget(secondsBox_);

    const bool meters = (shortUnit_ == DepthUnit::Meters);
    auto* distLbl = new QLabel(QStringLiteral("Distance"));
    distLbl->setStyleSheet(QStringLiteral("font-size:14px; color:%1;").arg(t.actionFg));
    col->addWidget(distLbl);
    distanceBox_ = new TouchSpinBox;
    // 0 means "no distance gate": record on every interval, wherever the boat is.
    distanceBox_->setRange(0.0, meters ? 3000.0 : 10000.0);
    distanceBox_->setSingleStep(meters ? 5.0 : 10.0);
    distanceBox_->setDecimals(0);
    distanceBox_->setSuffix(QLatin1Char(' ') + units::shortDistanceUnitKey(shortUnit_));
    distanceBox_->setValue(units::shortDistanceFromNm(minDistanceNm, shortUnit_));
    col->addWidget(distanceBox_);

    panelCol->addWidget(body);
    panelCol->addStretch(1);
    panelCol->addWidget(dialogchrome::okCancelRow(this));
}

double TrackingDialog::intervalSeconds() const { return secondsBox_->value(); }

double TrackingDialog::minDistanceNm() const {
    return units::nmFromShortDistance(distanceBox_->value(), shortUnit_);
}
