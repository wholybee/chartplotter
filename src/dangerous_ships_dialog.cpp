#include "dangerous_ships_dialog.hpp"
#include "touch_spin_box.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QCheckBox>
#include <QFrame>
#include <QPushButton>
#include <QWidget>

namespace {
QFrame* makeDivider() {
    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Plain);
    line->setStyleSheet(QStringLiteral("color:%1;").arg(theme::menu().separator));
    return line;
}

// A rule row: an enable checkbox with a touch stepper indented beneath it.
// Enable wiring is done by the caller so rows can gate one another. Returns the
// assembled widget; the caller keeps the checkbox/box pointers for read-back.
QWidget* ruleRow(QCheckBox* check, TouchSpinBox* box) {
    auto* w = new QWidget;
    auto* col = new QVBoxLayout(w);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(4);
    check->setStyleSheet(QStringLiteral("font-size:14px; color:%1;").arg(theme::menu().actionFg));
    col->addWidget(check);

    // Indent the stepper so it reads as belonging to the checkbox above it.
    auto* indent = new QHBoxLayout;
    indent->setContentsMargins(24, 0, 0, 0);
    indent->addWidget(box);
    col->addLayout(indent);
    return w;
}
} // namespace

DangerousShipsDialog::DangerousShipsDialog(bool ignoreFarEnabled, double ignoreFarNm,
                                           bool cpaEnabled, double cpaNm,
                                           bool tcpaEnabled, double tcpaMin,
                                           bool anchoredSafeEnabled, double anchoredSogKn,
                                           bool alarmSoundEnabled,
                                           QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Dangerous Ships"));
    resize(460, 680);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Dangerous Ships"));

    auto* body = new QWidget;
    auto* col = new QVBoxLayout(body);
    col->setContentsMargins(16, 12, 16, 12);
    col->setSpacing(14);

    // ---- Pre-filter ---------------------------------------------------------
    ignoreFarCheck_ = new QCheckBox(
        QStringLiteral("Ignore CPA for targets greater than (nm)"));
    ignoreFarCheck_->setChecked(ignoreFarEnabled);
    ignoreFarBox_ = new TouchSpinBox;
    ignoreFarBox_->setRange(0.1, 100.0);
    ignoreFarBox_->setSingleStep(1.0);
    ignoreFarBox_->setDecimals(1);
    ignoreFarBox_->setSuffix(QStringLiteral(" nm"));
    ignoreFarBox_->setValue(ignoreFarNm);
    col->addWidget(ruleRow(ignoreFarCheck_, ignoreFarBox_));

    col->addWidget(makeDivider());

    // ---- Dangerous-if rules -------------------------------------------------
    auto* heading = new QLabel(QStringLiteral("A ship is dangerous if:"));
    heading->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(theme::textMuted()));
    col->addWidget(heading);

    cpaCheck_ = new QCheckBox(QStringLiteral("CPA is less than (nm)"));
    cpaCheck_->setChecked(cpaEnabled);
    cpaBox_ = new TouchSpinBox;
    cpaBox_->setRange(0.1, 50.0);
    cpaBox_->setSingleStep(0.1);
    cpaBox_->setDecimals(1);
    cpaBox_->setSuffix(QStringLiteral(" nm"));
    cpaBox_->setValue(cpaNm);
    col->addWidget(ruleRow(cpaCheck_, cpaBox_));

    tcpaCheck_ = new QCheckBox(QStringLiteral("…and TCPA is less than (min)"));
    tcpaCheck_->setChecked(tcpaEnabled);
    tcpaBox_ = new TouchSpinBox;
    tcpaBox_->setRange(1.0, 120.0);
    tcpaBox_->setSingleStep(1.0);
    tcpaBox_->setDecimals(0);
    tcpaBox_->setSuffix(QStringLiteral(" min"));
    tcpaBox_->setValue(tcpaMin);
    col->addWidget(ruleRow(tcpaCheck_, tcpaBox_));

    // Enable wiring. The ignore-far stepper simply follows its checkbox. The CPA
    // checkbox is the base "dangerous" condition, so when it is off the whole
    // TCPA row is irrelevant and greyed out; the TCPA stepper needs both its own
    // checkbox and the CPA checkbox on.
    ignoreFarBox_->setEnabled(ignoreFarCheck_->isChecked());
    connect(ignoreFarCheck_, &QCheckBox::toggled, ignoreFarBox_, &TouchSpinBox::setEnabled);

    auto syncDangerIf = [this] {
        const bool cpaOn = cpaCheck_->isChecked();
        cpaBox_->setEnabled(cpaOn);
        tcpaCheck_->setEnabled(cpaOn);
        tcpaBox_->setEnabled(cpaOn && tcpaCheck_->isChecked());
    };
    connect(cpaCheck_,  &QCheckBox::toggled, this, [syncDangerIf](bool) { syncDangerIf(); });
    connect(tcpaCheck_, &QCheckBox::toggled, this, [syncDangerIf](bool) { syncDangerIf(); });
    syncDangerIf();

    col->addWidget(makeDivider());

    // ---- Anchored-safe override ---------------------------------------------
    // Treat stationary vessels as never dangerous, to clear the false-positive
    // swarm from a marina or anchorage.
    anchoredCheck_ = new QCheckBox(QStringLiteral("Consider anchored vessels safe"));
    anchoredCheck_->setStyleSheet(QStringLiteral("font-size:14px; color:%1;").arg(t.actionFg));
    anchoredCheck_->setChecked(anchoredSafeEnabled);
    col->addWidget(anchoredCheck_);

    auto* anchoredHint = new QLabel(
        QStringLiteral("Anchored if AIS status is \"At anchor\", or SOG is at or below:"));
    anchoredHint->setWordWrap(true);
    anchoredHint->setStyleSheet(QStringLiteral("font-size:12px; color:%1;").arg(theme::textMuted()));

    anchoredSogBox_ = new TouchSpinBox;
    anchoredSogBox_->setRange(0.0, 5.0);
    anchoredSogBox_->setSingleStep(0.1);
    anchoredSogBox_->setDecimals(1);
    anchoredSogBox_->setSuffix(QStringLiteral(" kn"));
    anchoredSogBox_->setValue(anchoredSogKn);

    auto* anchoredIndent = new QVBoxLayout;
    anchoredIndent->setContentsMargins(24, 0, 0, 0);
    anchoredIndent->setSpacing(4);
    anchoredIndent->addWidget(anchoredHint);
    {
        auto* sogRow = new QHBoxLayout;
        sogRow->addWidget(anchoredSogBox_);
        sogRow->addStretch(1);
        anchoredIndent->addLayout(sogRow);
    }
    col->addLayout(anchoredIndent);

    // The SOG threshold (and its hint) only matter while the override is on.
    auto syncAnchored = [this, anchoredHint] {
        const bool on = anchoredCheck_->isChecked();
        anchoredSogBox_->setEnabled(on);
        anchoredHint->setEnabled(on);
    };
    connect(anchoredCheck_, &QCheckBox::toggled, this, [syncAnchored](bool) { syncAnchored(); });
    syncAnchored();

    col->addWidget(makeDivider());

    // ---- Audible alarm ------------------------------------------------------
    // Sound a repeating beep whenever a target is dangerous, loud enough to wake
    // a resting watchkeeper. Off by default.
    alarmCheck_ = new QCheckBox(QStringLiteral("Sound an alarm when a ship is dangerous"));
    alarmCheck_->setStyleSheet(QStringLiteral("font-size:14px; color:%1;").arg(t.actionFg));
    alarmCheck_->setChecked(alarmSoundEnabled);
    col->addWidget(alarmCheck_);

    auto* alarmHint = new QLabel(
        QStringLiteral("Repeats until no dangerous ship remains."));
    alarmHint->setWordWrap(true);
    alarmHint->setStyleSheet(QStringLiteral("font-size:12px; color:%1;").arg(theme::textMuted()));
    {
        auto* alarmIndent = new QVBoxLayout;
        alarmIndent->setContentsMargins(24, 0, 0, 0);
        alarmIndent->addWidget(alarmHint);
        col->addLayout(alarmIndent);
    }

    col->addStretch(1);
    panelCol->addWidget(body, 1);
    panelCol->addWidget(dialogchrome::okCancelRow(this));
}

bool   DangerousShipsDialog::ignoreFarEnabled() const { return ignoreFarCheck_->isChecked(); }
double DangerousShipsDialog::ignoreFarNm()      const { return ignoreFarBox_->value(); }
bool   DangerousShipsDialog::cpaEnabled()  const { return cpaCheck_->isChecked(); }
double DangerousShipsDialog::cpaNm()       const { return cpaBox_->value(); }
bool   DangerousShipsDialog::tcpaEnabled() const { return tcpaCheck_->isChecked(); }
double DangerousShipsDialog::tcpaMin()     const { return tcpaBox_->value(); }
bool   DangerousShipsDialog::anchoredSafeEnabled() const { return anchoredCheck_->isChecked(); }
double DangerousShipsDialog::anchoredSogKn()       const { return anchoredSogBox_->value(); }
bool   DangerousShipsDialog::alarmSoundEnabled()   const { return alarmCheck_->isChecked(); }
