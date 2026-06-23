#pragma once
#include "theme.hpp"
#include "window_dragger.hpp"

#include <QDialog>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QLineEdit>
#include <QCheckBox>
#include <QRadioButton>

// Shared "frameless, side-menu palette" chrome for the touch dialogs, so they
// all match the main menu and the AIS Targets list without each one repeating
// the title-bar / panel / button boilerplate. Header-only (no Q_OBJECT, no moc),
// like window_dragger.hpp.
//
// Typical use:
//     auto* panelCol = dialogchrome::setup(this, "Ship Size");
//     panelCol->addWidget(dialogchrome::sectionHeader("Vessel Size"));
//     panelCol->addWidget(body);          // your margined content widget
//     panelCol->addStretch(1);
//     panelCol->addWidget(dialogchrome::okCancelRow(this));
namespace dialogchrome {

// Turn `dlg` into a frameless, themed dialog with a navy title bar (draggable,
// with a ✕). Returns the vertical layout inside the bordered panel; add the body
// content and a button row to it.
//
// The ✕ rejects by default. Pass closeOnDismiss=true for a modeless, self-
// deleting dialog (Qt::WA_DeleteOnClose), where dismissal must go through
// close() so the close event fires and the dialog is destroyed — reject() would
// only hide it.
inline QVBoxLayout* setup(QDialog* dlg, const QString& title, bool closeOnDismiss = false) {
    const theme::MenuPalette& t = theme::menu();
    dlg->setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);

    auto* outer = new QVBoxLayout(dlg);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Bordered panel so the frameless window still has a visible edge.
    auto* panel = new QFrame(dlg);
    panel->setObjectName(QStringLiteral("ChromePanel"));
    panel->setStyleSheet(QStringLiteral(
        "#ChromePanel{ background:%1; border:1px solid %2; }").arg(t.panelBg, t.panelBorder));
    outer->addWidget(panel);

    auto* panelCol = new QVBoxLayout(panel);
    panelCol->setContentsMargins(0, 0, 0, 0);
    panelCol->setSpacing(0);

    // Title bar (brand-navy), draggable, with a close "✕" that cancels.
    auto* titleBar = new QWidget(panel);
    titleBar->setStyleSheet(QStringLiteral("background:%1;").arg(t.titleBg));
    titleBar->setCursor(Qt::SizeAllCursor);
    titleBar->installEventFilter(new WindowDragger(dlg));
    auto* titleRow = new QHBoxLayout(titleBar);
    titleRow->setContentsMargins(16, 8, 8, 8);
    titleRow->setSpacing(6);
    auto* titleLbl = new QLabel(title, titleBar);
    titleLbl->setAttribute(Qt::WA_TransparentForMouseEvents);   // clicks fall to the bar (drag)
    titleLbl->setStyleSheet(QStringLiteral(
        "font-size:18px; font-weight:600; background:transparent; color:%1;").arg(t.titleFg));
    titleRow->addWidget(titleLbl, 1);
    auto* close = new QPushButton(QString(QChar(0x2715)), titleBar);   // ✕
    close->setFlat(true);
    close->setFixedSize(44, 44);
    close->setCursor(Qt::PointingHandCursor);
    close->setStyleSheet(QStringLiteral(
        "QPushButton{ border:none; background:transparent; color:%1; font-size:18px; }"
        "QPushButton:pressed{ background:%2; border-radius:6px; }").arg(t.titleFg, t.actionPressed));
    if (closeOnDismiss) QObject::connect(close, &QPushButton::clicked, dlg, &QWidget::close);
    else                QObject::connect(close, &QPushButton::clicked, dlg, &QDialog::reject);
    titleRow->addWidget(close);
    panelCol->addWidget(titleBar);

    return panelCol;
}

// Full-width section header strip, mirroring the side-menu section headers.
inline QLabel* sectionHeader(const QString& text) {
    const theme::MenuPalette& t = theme::menu();
    auto* h = new QLabel(text);
    h->setStyleSheet(QStringLiteral(
        "font-size:12px; font-weight:600; color:%1;"
        "padding:14px 16px 6px 16px; background:%2;").arg(t.headerFg, t.headerBg));
    return h;
}

// Accent-filled primary button (e.g. OK). Dims when disabled.
inline void styleAccentButton(QPushButton* b) {
    const theme::MenuPalette& t = theme::menu();
    b->setMinimumHeight(44);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral(
        "QPushButton{ border:none; background:%1; color:%2;"
        " border-radius:6px; padding:0 22px; font-size:15px; font-weight:600; }"
        "QPushButton:pressed{ background:%3; }"
        "QPushButton:disabled{ background:%4; color:%5; }")
        .arg(t.accent, t.titleFg, t.titleBg, t.actionPressed, theme::textMuted()));
}

// Outlined secondary button (e.g. Cancel).
inline void styleOutlinedButton(QPushButton* b) {
    const theme::MenuPalette& t = theme::menu();
    b->setMinimumHeight(44);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral(
        "QPushButton{ border:1px solid %1; background:%2; color:%3;"
        " border-radius:6px; padding:0 18px; font-size:15px; }"
        "QPushButton:pressed{ background:%4; }"
        "QPushButton:disabled{ color:%5; }")
        .arg(t.separator, t.panelBg, t.actionFg, t.actionPressed, theme::textMuted()));
}

// Touch-sized text field, themed to the input palette with an accent focus ring.
inline void styleLineEdit(QLineEdit* e) {
    const theme::MenuPalette& t = theme::menu();
    const theme::InputPalette& in = theme::input();
    e->setMinimumHeight(44);
    e->setStyleSheet(QStringLiteral(
        "QLineEdit{ font-size:16px; padding:6px 10px; background:%1; color:%2;"
        " border:1px solid %3; border-radius:6px; }"
        "QLineEdit:focus{ border:1px solid %4; }")
        .arg(in.fieldBg, in.fg, in.border, t.accent));
}

// Touch-sized checkbox, themed text + a larger indicator for finger targets.
inline void styleCheckBox(QCheckBox* c) {
    const theme::MenuPalette& t = theme::menu();
    c->setMinimumHeight(44);
    c->setCursor(Qt::PointingHandCursor);
    c->setStyleSheet(QStringLiteral(
        "QCheckBox{ font-size:15px; color:%1; spacing:8px; }"
        "QCheckBox::indicator{ width:22px; height:22px; }").arg(t.actionFg));
}

// Touch-sized radio button, themed to match.
inline void styleRadio(QRadioButton* r) {
    const theme::MenuPalette& t = theme::menu();
    r->setMinimumHeight(40);
    r->setCursor(Qt::PointingHandCursor);
    r->setStyleSheet(QStringLiteral(
        "QRadioButton{ font-size:15px; color:%1; spacing:8px; }"
        "QRadioButton::indicator{ width:22px; height:22px; }").arg(t.actionFg));
}

// A horizontal slider styled to the palette: neutral groove, accent-blue handle.
inline QSlider* styledSlider(int lo, int hi) {
    const theme::MenuPalette& t = theme::menu();
    auto* s = new QSlider(Qt::Horizontal);
    s->setMinimum(lo);
    s->setMaximum(hi);
    s->setSingleStep(1);
    s->setPageStep(1);
    s->setTickPosition(QSlider::TicksBelow);
    s->setTickInterval(1);
    s->setMinimumHeight(44);
    s->setCursor(Qt::PointingHandCursor);
    s->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal{ height:6px; background:%1; border-radius:3px; }"
        "QSlider::handle:horizontal{ background:%2; width:24px; height:24px;"
        " margin:-10px 0; border-radius:12px; }"
        "QSlider::handle:horizontal:pressed{ background:%3; }")
        .arg(t.separator, t.accent, t.titleBg));
    return s;
}

// Themed Cancel/OK row wired to reject()/accept(). Optionally hands back the OK
// button (for enable-state control). Add the returned widget to the panel layout.
inline QWidget* okCancelRow(QDialog* dlg, QPushButton** okOut = nullptr) {
    auto* bar = new QWidget;
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(16, 8, 16, 16);
    row->setSpacing(10);
    auto* cancel = new QPushButton(QStringLiteral("Cancel"), bar);
    auto* ok     = new QPushButton(QStringLiteral("OK"), bar);
    styleOutlinedButton(cancel);
    styleAccentButton(ok);
    ok->setDefault(true);
    row->addStretch(1);
    row->addWidget(cancel);
    row->addWidget(ok);
    QObject::connect(cancel, &QPushButton::clicked, dlg, &QDialog::reject);
    QObject::connect(ok,     &QPushButton::clicked, dlg, &QDialog::accept);
    if (okOut) *okOut = ok;
    return bar;
}

} // namespace dialogchrome
