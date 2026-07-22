#include "chart_symbol_size_dialog.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QCheckBox>
#include <QWidget>
#include <cmath>

namespace {
// Size sliders run 2..12 where each step = 25 %. step 4 = 100 % = scale 1.0.
constexpr int kSliderMin = 2;
constexpr int kSliderMax = 12;

// Nudge-distance slider runs 1..8 where each step = 5 px (5 .. 40 px).
constexpr int    kNudgeMin  = 1;
constexpr int    kNudgeMax  = 8;
constexpr double kNudgePxPerStep = 5.0;

int    scaleToStep(double scale) { return static_cast<int>(std::lround(scale / 0.25)); }
double stepToScale(int step)     { return step * 0.25; }

int    pxToStep(double px)  { return static_cast<int>(std::lround(px / kNudgePxPerStep)); }
double stepToPx(int step)   { return step * kNudgePxPerStep; }

QString formatScale(double scale) {
    int pct = static_cast<int>(std::lround(scale * 100.0));
    if (pct == 100) return QStringLiteral("100 % (normal)");
    return QString::number(pct) + QStringLiteral(" %");
}

// One labelled slider block: an intro line, the slider (seeded from `scale`),
// and a live percentage read-out under it. Appends the widgets to `col` and
// returns the slider so the caller can read it back.
QSlider* addScaleControl(QVBoxLayout* col, const QString& intro, double scale) {
    const theme::MenuPalette& t = theme::menu();

    auto* lbl = new QLabel(intro);
    lbl->setWordWrap(true);
    lbl->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(t.actionFg));
    col->addWidget(lbl);

    auto* slider = dialogchrome::styledSlider(kSliderMin, kSliderMax);
    int step = scaleToStep(scale);
    if (step < kSliderMin) step = kSliderMin;
    if (step > kSliderMax) step = kSliderMax;
    slider->setValue(step);
    col->addWidget(slider);

    auto* value = new QLabel;
    value->setAlignment(Qt::AlignCenter);
    value->setStyleSheet(QStringLiteral(
        "font-size:14px; font-weight:600; color:%1;").arg(t.accent));
    col->addWidget(value);

    auto update = [value] (int v) { value->setText(formatScale(stepToScale(v))); };
    update(slider->value());
    QObject::connect(slider, &QSlider::valueChanged, value, update);
    return slider;
}

// Adds a margined body widget to the panel and returns its column layout.
QVBoxLayout* addSection(QVBoxLayout* panelCol, const QString& header) {
    panelCol->addWidget(dialogchrome::sectionHeader(header));
    auto* body = new QWidget;
    auto* col  = new QVBoxLayout(body);
    col->setContentsMargins(16, 4, 16, 12);
    col->setSpacing(10);
    panelCol->addWidget(body);
    return col;
}
} // namespace

ChartSymbolSizeDialog::ChartSymbolSizeDialog(double symbolScale, double textScale,
                                             double soundingScale, bool nudgeEnabled,
                                             double nudgeMaxPx, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Symbol Size"));
    resize(440, 720);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Symbol Size"));

    symbolSlider_ = addScaleControl(addSection(panelCol, QStringLiteral("Symbol Scale")),
        QStringLiteral(
        "Scale chart symbols relative to their normal size. Larger values make "
        "buoys, lights, and other point symbols easier to see at a glance."),
        symbolScale);

    textSlider_ = addScaleControl(addSection(panelCol, QStringLiteral("Text Size")),
        QStringLiteral(
        "Scale text labels such as place names and light descriptions."),
        textScale);

    soundingSlider_ = addScaleControl(addSection(panelCol, QStringLiteral("Sounding Size")),
        QStringLiteral(
        "Scale the depth-sounding numbers separately from other text."),
        soundingScale);

    // Text de-clutter: enable toggle + max nudge distance.
    auto* declutter = addSection(panelCol, QStringLiteral("Text Declutter"));
    nudgeCheck_ = new QCheckBox(QStringLiteral("Nudge labels to reduce overlap"));
    dialogchrome::styleCheckBox(nudgeCheck_);
    nudgeCheck_->setChecked(nudgeEnabled);
    declutter->addWidget(nudgeCheck_);

    auto* nudgeIntro = new QLabel(QStringLiteral(
        "Move overlapping text a little to keep it readable. Soundings and "
        "symbols stay put; labels avoid other labels first, then symbols, then "
        "soundings."));
    nudgeIntro->setWordWrap(true);
    nudgeIntro->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(t.actionFg));
    declutter->addWidget(nudgeIntro);

    nudgeSlider_ = dialogchrome::styledSlider(kNudgeMin, kNudgeMax);
    int nstep = pxToStep(nudgeMaxPx);
    if (nstep < kNudgeMin) nstep = kNudgeMin;
    if (nstep > kNudgeMax) nstep = kNudgeMax;
    nudgeSlider_->setValue(nstep);
    declutter->addWidget(nudgeSlider_);

    auto* nudgeValue = new QLabel;
    nudgeValue->setAlignment(Qt::AlignCenter);
    nudgeValue->setStyleSheet(QStringLiteral(
        "font-size:14px; font-weight:600; color:%1;").arg(t.accent));
    declutter->addWidget(nudgeValue);
    auto updateNudge = [nudgeValue] (int v) {
        nudgeValue->setText(QStringLiteral("max %1 px").arg(
            static_cast<int>(std::lround(stepToPx(v)))));
    };
    updateNudge(nudgeSlider_->value());
    connect(nudgeSlider_, &QSlider::valueChanged, nudgeValue, updateNudge);

    // The distance control is only meaningful while nudging is on.
    auto syncEnabled = [this, nudgeValue] (bool on) {
        nudgeSlider_->setEnabled(on);
        nudgeValue->setEnabled(on);
    };
    syncEnabled(nudgeCheck_->isChecked());
    connect(nudgeCheck_, &QCheckBox::toggled, this, syncEnabled);

    panelCol->addStretch(1);
    panelCol->addWidget(dialogchrome::okCancelRow(this));
}

double ChartSymbolSizeDialog::symbolScale() const {
    return stepToScale(symbolSlider_->value());
}

double ChartSymbolSizeDialog::textScale() const {
    return stepToScale(textSlider_->value());
}

double ChartSymbolSizeDialog::soundingScale() const {
    return stepToScale(soundingSlider_->value());
}

bool ChartSymbolSizeDialog::nudgeEnabled() const {
    return nudgeCheck_->isChecked();
}

double ChartSymbolSizeDialog::nudgeMaxPx() const {
    return stepToPx(nudgeSlider_->value());
}
