#include "stale_thresholds_dialog.hpp"
#include "touch_spin_box.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QLabel>
#include <QWidget>
#include <algorithm>

namespace {
// A titled value row: a small caption above a touch stepper.
QWidget* labelledStepper(const QString& caption, TouchSpinBox* box) {
    auto* w = new QWidget;
    auto* col = new QVBoxLayout(w);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(4);
    auto* cap = new QLabel(caption);
    cap->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(theme::textMuted()));
    cap->setWordWrap(true);
    col->addWidget(cap);
    col->addWidget(box);
    return w;
}
} // namespace

StaleThresholdsDialog::StaleThresholdsDialog(double staleSeconds, double invalidSeconds,
                                             double aisStaleSeconds, double aisLostSeconds,
                                             QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Stale Data Thresholds"));
    resize(440, 560);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Stale Data Thresholds"));

    auto intro = [&](const QString& text) {
        auto* l = new QLabel(text);
        l->setWordWrap(true);
        l->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(t.actionFg));
        return l;
    };

    // ---- Ownship section ----------------------------------------------------
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Ownship")));
    auto* ownBody = new QWidget;
    auto* ownCol  = new QVBoxLayout(ownBody);
    ownCol->setContentsMargins(16, 4, 16, 12);
    ownCol->setSpacing(10);

    ownCol->addWidget(intro(QStringLiteral(
        "How long an ownship fix is trusted before it is flagged as stale, "
        "then hidden.")));

    staleBox_ = new TouchSpinBox;
    staleBox_->setRange(1.0, 600.0);
    staleBox_->setSingleStep(1.0);
    staleBox_->setDecimals(0);
    staleBox_->setSuffix(QStringLiteral(" s"));
    staleBox_->setValue(staleSeconds);
    ownCol->addWidget(labelledStepper(
        QStringLiteral("Ownship — mark Stale after:"), staleBox_));

    invalidBox_ = new TouchSpinBox;
    invalidBox_->setRange(staleSeconds + 1.0, 3600.0);
    invalidBox_->setSingleStep(1.0);
    invalidBox_->setDecimals(0);
    invalidBox_->setSuffix(QStringLiteral(" s"));
    invalidBox_->setValue(std::max(invalidSeconds, staleSeconds + 1.0));
    ownCol->addWidget(labelledStepper(
        QStringLiteral("Ownship — mark Invalid (hidden) after:"), invalidBox_));

    connect(staleBox_, &TouchSpinBox::valueChanged, this, [this](double s) {
        invalidBox_->setRange(s + 1.0, 3600.0);
    });
    panelCol->addWidget(ownBody);

    // ---- AIS section --------------------------------------------------------
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("AIS Targets")));
    auto* aisBody = new QWidget;
    auto* aisCol  = new QVBoxLayout(aisBody);
    aisCol->setContentsMargins(16, 4, 16, 12);
    aisCol->setSpacing(10);

    aisCol->addWidget(intro(QStringLiteral(
        "How long an AIS target is kept before it is greyed out, then "
        "removed from the display.")));

    const double aisStaleMin = aisStaleSeconds / 60.0;
    const double aisLostMin  = aisLostSeconds  / 60.0;

    aisStaleBox_ = new TouchSpinBox;
    aisStaleBox_->setRange(1.0, 60.0);
    aisStaleBox_->setSingleStep(1.0);
    aisStaleBox_->setDecimals(0);
    aisStaleBox_->setSuffix(QStringLiteral(" min"));
    aisStaleBox_->setValue(aisStaleMin);
    aisCol->addWidget(labelledStepper(
        QStringLiteral("AIS — mark Stale after:"), aisStaleBox_));

    aisLostBox_ = new TouchSpinBox;
    aisLostBox_->setRange(aisStaleMin + 1.0, 120.0);
    aisLostBox_->setSingleStep(1.0);
    aisLostBox_->setDecimals(0);
    aisLostBox_->setSuffix(QStringLiteral(" min"));
    aisLostBox_->setValue(std::max(aisLostMin, aisStaleMin + 1.0));
    aisCol->addWidget(labelledStepper(
        QStringLiteral("AIS — remove after:"), aisLostBox_));

    connect(aisStaleBox_, &TouchSpinBox::valueChanged, this, [this](double s) {
        aisLostBox_->setRange(s + 1.0, 120.0);
    });
    panelCol->addWidget(aisBody);

    panelCol->addStretch(1);
    panelCol->addWidget(dialogchrome::okCancelRow(this));
}

double StaleThresholdsDialog::staleSeconds()    const { return staleBox_->value(); }
double StaleThresholdsDialog::invalidSeconds()  const { return invalidBox_->value(); }
double StaleThresholdsDialog::aisStaleSeconds() const { return aisStaleBox_->value() * 60.0; }
double StaleThresholdsDialog::aisLostSeconds()  const { return aisLostBox_->value()  * 60.0; }
