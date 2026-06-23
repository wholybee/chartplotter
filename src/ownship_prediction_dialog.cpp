#include "ownship_prediction_dialog.hpp"
#include "touch_spin_box.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QLabel>
#include <QWidget>

OwnshipPredictionDialog::OwnshipPredictionDialog(double minutes, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Ownship Course Prediction"));
    resize(440, 280);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Ownship Course Prediction"));
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Prediction Length")));

    auto* body = new QWidget;
    auto* col = new QVBoxLayout(body);
    col->setContentsMargins(16, 4, 16, 12);
    col->setSpacing(10);

    auto* intro = new QLabel(QStringLiteral(
        "How far ahead the course-prediction line reaches, as minutes of travel "
        "at the current speed."));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(t.actionFg));
    col->addWidget(intro);

    minutesBox_ = new TouchSpinBox;
    minutesBox_->setRange(1.0, 120.0);
    minutesBox_->setSingleStep(1.0);
    minutesBox_->setDecimals(0);
    minutesBox_->setSuffix(QStringLiteral(" min"));
    minutesBox_->setValue(minutes);
    col->addWidget(minutesBox_);
    panelCol->addWidget(body);

    panelCol->addStretch(1);
    panelCol->addWidget(dialogchrome::okCancelRow(this));
}

double OwnshipPredictionDialog::minutes() const { return minutesBox_->value(); }
