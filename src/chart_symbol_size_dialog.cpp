#include "chart_symbol_size_dialog.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QWidget>
#include <cmath>

namespace {
// Slider runs 2..12 where each step = 25 %. step 4 = 100 % = scale 1.0.
constexpr int kSliderMin = 2;
constexpr int kSliderMax = 12;

int    scaleToStep(double scale) { return static_cast<int>(std::lround(scale / 0.25)); }
double stepToScale(int step)     { return step * 0.25; }

QString formatScale(double scale) {
    int pct = static_cast<int>(std::lround(scale * 100.0));
    if (pct == 100) return QStringLiteral("100 % (normal)");
    return QString::number(pct) + QStringLiteral(" %");
}
} // namespace

ChartSymbolSizeDialog::ChartSymbolSizeDialog(double scale, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Symbol Size"));
    resize(440, 300);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Symbol Size"));
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Symbol Scale")));

    auto* body = new QWidget;
    auto* col = new QVBoxLayout(body);
    col->setContentsMargins(16, 4, 16, 12);
    col->setSpacing(10);

    auto* intro = new QLabel(QStringLiteral(
        "Scale chart symbols relative to their normal size. "
        "Larger values make buoys, lights, and other point symbols "
        "easier to see at a glance."));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(t.actionFg));
    col->addWidget(intro);

    slider_ = dialogchrome::styledSlider(kSliderMin, kSliderMax);
    int step = scaleToStep(scale);
    if (step < kSliderMin) step = kSliderMin;
    if (step > kSliderMax) step = kSliderMax;
    slider_->setValue(step);
    col->addWidget(slider_);

    valueLabel_ = new QLabel;
    valueLabel_->setAlignment(Qt::AlignCenter);
    valueLabel_->setStyleSheet(QStringLiteral(
        "font-size:14px; font-weight:600; color:%1;").arg(t.accent));
    col->addWidget(valueLabel_);
    updateValueLabel();
    connect(slider_, &QSlider::valueChanged, this, [this] { updateValueLabel(); });
    panelCol->addWidget(body);

    panelCol->addStretch(1);
    panelCol->addWidget(dialogchrome::okCancelRow(this));
}

double ChartSymbolSizeDialog::symbolScale() const {
    return stepToScale(slider_->value());
}

void ChartSymbolSizeDialog::updateValueLabel() {
    valueLabel_->setText(formatScale(stepToScale(slider_->value())));
}
