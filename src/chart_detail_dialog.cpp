#include "chart_detail_dialog.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QWidget>
#include <cmath>

namespace {
// The detail slider runs in integer half-band steps so the tick marks land
// exactly on the requested stops. Range -2..+2 maps to -1.0, -0.5, 0.0, +0.5,
// +1.0.
constexpr int kSliderMin = -2;
constexpr int kSliderMax =  2;

int    levelToStep(double level) { return static_cast<int>(std::lround(level)); }
double stepToLevel(int step)     { return static_cast<double>(step); }

QString formatLevel(double level) {
    if (level == 0.0) return QStringLiteral("0 (normal)");
    return (level > 0.0 ? QStringLiteral("+") : QString())
         + QString::number(level, 'f', 1);
}

// The SCAMIN slider has 9 integer stops; each maps to one quarter, so the range
// -4..+4 covers -1.0 .. +1.0 (the level Settings/ChartView expect). The end
// stops are the hard "hide all" / "show all" cases.
constexpr int kScaminStepMin = -4;
constexpr int kScaminStepMax =  4;

int    scaminLevelToStep(double level) {
    return static_cast<int>(std::lround(level * 4.0));
}
double scaminStepToLevel(int step) { return step / 4.0; }

QString formatScamin(int step) {
    if (step <= kScaminStepMin) return QStringLiteral("Hide all objects");
    if (step >= kScaminStepMax) return QStringLiteral("Show all objects");
    if (step == 0)              return QStringLiteral("Auto — by zoom");
    return step > 0 ? QStringLiteral("More objects (+%1)").arg(step)
                    : QStringLiteral("Fewer objects (%1)").arg(step);  // step is negative
}
} // namespace

ChartDetailDialog::ChartDetailDialog(double detailLevel, double scaminLevel,
                                     QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Chart Detail"));
    resize(460, 560);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Chart Detail"));

    auto bodyLabel = [&](const QString& text) {
        auto* l = new QLabel(text);
        l->setWordWrap(true);
        l->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(t.actionFg));
        return l;
    };
    auto valueLabel = [&]() {
        auto* l = new QLabel;
        l->setAlignment(Qt::AlignCenter);
        l->setStyleSheet(QStringLiteral(
            "font-size:14px; font-weight:600; color:%1;").arg(t.accent));
        return l;
    };

    // --- Object detail (SCAMIN declutter) — the top control -----------------
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Object Detail")));
    auto* objBody = new QWidget;
    auto* objCol  = new QVBoxLayout(objBody);
    objCol->setContentsMargins(16, 4, 16, 12);
    objCol->setSpacing(8);

    objCol->addWidget(bodyLabel(QStringLiteral(
        "Control how soon individual objects (buoys, beacons, soundings, …) "
        "drop off as you zoom out, using each object's built-in SCAMIN scale. "
        "Left of centre declutters; right of centre keeps more on screen. The "
        "ends hide or show all objects regardless of zoom.")));

    scaminSlider_ = dialogchrome::styledSlider(kScaminStepMin, kScaminStepMax);
    int sStep = scaminLevelToStep(scaminLevel);
    if (sStep < kScaminStepMin) sStep = kScaminStepMin;
    if (sStep > kScaminStepMax) sStep = kScaminStepMax;
    scaminSlider_->setValue(sStep);
    objCol->addWidget(scaminSlider_);

    scaminValueLabel_ = valueLabel();
    objCol->addWidget(scaminValueLabel_);
    updateScaminLabel();
    connect(scaminSlider_, &QSlider::valueChanged, this, [this] { updateScaminLabel(); });
    panelCol->addWidget(objBody);

    // --- Detail bias (chart-band selection) — below -------------------------
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Detail Bias")));
    auto* biasBody = new QWidget;
    auto* biasCol  = new QVBoxLayout(biasBody);
    biasCol->setContentsMargins(16, 4, 16, 12);
    biasCol->setSpacing(8);

    biasCol->addWidget(bodyLabel(QStringLiteral(
        "Adjust how much chart detail is shown at the current zoom. Higher "
        "values pull in higher-detail charts; lower values back off to less "
        "detail. Zoom is unchanged.")));

    slider_ = dialogchrome::styledSlider(kSliderMin, kSliderMax);
    int step = levelToStep(detailLevel);
    if (step < kSliderMin) step = kSliderMin;
    if (step > kSliderMax) step = kSliderMax;
    slider_->setValue(step);
    biasCol->addWidget(slider_);

    valueLabel_ = valueLabel();
    biasCol->addWidget(valueLabel_);
    updateDetailLabel();
    connect(slider_, &QSlider::valueChanged, this, [this] { updateDetailLabel(); });
    panelCol->addWidget(biasBody);

    panelCol->addStretch(1);
    panelCol->addWidget(dialogchrome::okCancelRow(this));
}

double ChartDetailDialog::detailLevel() const {
    return stepToLevel(slider_->value());
}

double ChartDetailDialog::scaminLevel() const {
    return scaminStepToLevel(scaminSlider_->value());
}

void ChartDetailDialog::updateDetailLabel() {
    valueLabel_->setText(formatLevel(stepToLevel(slider_->value())));
}

void ChartDetailDialog::updateScaminLabel() {
    scaminValueLabel_->setText(formatScamin(scaminSlider_->value()));
}
