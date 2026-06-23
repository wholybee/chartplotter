#include "navigation_options_dialog.hpp"
#include "touch_spin_box.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QLabel>
#include <QWidget>

NavigationOptionsDialog::NavigationOptionsDialog(double arrivalRadiusNm, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Navigation Options"));
    resize(440, 280);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Navigation Options"));
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Arrival Radius")));

    auto* body = new QWidget;
    auto* bodyCol = new QVBoxLayout(body);
    bodyCol->setContentsMargins(16, 4, 16, 12);
    bodyCol->setSpacing(10);

    auto* intro = new QLabel(QStringLiteral(
        "How close the boat must come to a waypoint, in nautical miles, for it to "
        "count as arrived."));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(t.actionFg));
    bodyCol->addWidget(intro);

    arrivalBox_ = new TouchSpinBox;
    arrivalBox_->setRange(0.001, 10.0);
    arrivalBox_->setDecimals(3);          // precision 0.001 nm
    arrivalBox_->setSingleStep(0.01);
    arrivalBox_->setSuffix(QStringLiteral(" nm"));
    arrivalBox_->setValue(arrivalRadiusNm);
    bodyCol->addWidget(arrivalBox_);
    panelCol->addWidget(body);

    panelCol->addStretch(1);
    panelCol->addWidget(dialogchrome::okCancelRow(this));
}

double NavigationOptionsDialog::arrivalRadiusNm() const { return arrivalBox_->value(); }
