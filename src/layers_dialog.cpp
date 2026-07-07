#include "layers_dialog.hpp"
#include "settings.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

namespace {
// Match the dark info-panel type colours (see ais_target_info_window.cpp).
const QString kText = QStringLiteral("#e6e9ee");
const QString kDim  = QStringLiteral("rgba(230,233,238,150)");
} // namespace

LayersDialog::LayersDialog(Settings* settings, QWidget* parent)
    : FramelessInfoDialog(parent), settings_(settings) {
    setWindowTitle(QStringLiteral("Layers"));

    auto* col = panelLayout();

    // ---- Header: title + close (this window is frameless).
    auto* header = new QHBoxLayout;
    header->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("Layers"), this);
    title->setStyleSheet(QStringLiteral("font-size:18px; font-weight:700; color:%1;").arg(kText));
    header->addWidget(title, 1);
    header->addWidget(makeCloseButton(), 0, Qt::AlignTop);
    col->addLayout(header);

    // ---- Toggle rows. Each is wired to a Settings flag both ways: the toggle
    // drives the setter, and the matching change signal drives the toggle, so a
    // change made from the side menu (or anywhere) is reflected here too. The
    // setters no-op on an unchanged value, so this can't loop.
    QPushButton* snd = makeToggle(QStringLiteral("Soundings"), settings_->showSoundings());
    connect(snd, &QPushButton::toggled, settings_, &Settings::setShowSoundings);
    connect(settings_, &Settings::showSoundingsChanged, snd, &QPushButton::setChecked);

    QPushButton* sym = makeToggle(QStringLiteral("Symbols"), settings_->showSymbols());
    connect(sym, &QPushButton::toggled, settings_, &Settings::setShowSymbols);
    connect(settings_, &Settings::showSymbolsChanged, sym, &QPushButton::setChecked);

    QPushButton* txt = makeToggle(QStringLiteral("Text"), settings_->showText());
    connect(txt, &QPushButton::toggled, settings_, &Settings::setShowText);
    connect(settings_, &Settings::showTextChanged, txt, &QPushButton::setChecked);

    QPushButton* con = makeToggle(QStringLiteral("Depth Contours"), settings_->showDepthContours());
    connect(con, &QPushButton::toggled, settings_, &Settings::setShowDepthContours);
    connect(settings_, &Settings::showDepthContoursChanged, con, &QPushButton::setChecked);

    col->addStretch(1);
}

QPushButton* LayersDialog::makeToggle(const QString& label, bool on) {
    auto* b = new QPushButton(this);
    b->setCheckable(true);
    b->setMinimumHeight(52);   // touch target
    b->setCursor(Qt::PointingHandCursor);
    b->setFocusPolicy(Qt::NoFocus);

    // A leading tick when on, blank when off — same on/off cue the menu uses.
    auto sync = [b, label](bool checked) {
        b->setText((checked ? QStringLiteral("  ✓   ") : QStringLiteral("       ")) + label);
    };
    sync(on);
    b->setChecked(on);
    connect(b, &QPushButton::toggled, b, sync);

    // Dim tile when off; brighter text on a blue-tinted tile when on.
    b->setStyleSheet(QStringLiteral(
        "QPushButton{ text-align:left; padding-left:10px; font-size:16px;"
        " border:1px solid rgba(255,255,255,40); border-radius:6px;"
        " background:rgba(255,255,255,18); color:%1; }"
        "QPushButton:checked{ color:%2; font-weight:600;"
        " background:rgba(74,132,200,60); border:1px solid rgba(120,170,230,120); }"
        "QPushButton:pressed{ background:rgba(255,255,255,36); }")
        .arg(kDim, kText));

    panelLayout()->addWidget(b);
    return b;
}
