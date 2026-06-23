#include "ownship_mmsi_dialog.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>
#include <QRegularExpressionValidator>

OwnshipMmsiDialog::OwnshipMmsiDialog(const QString& currentMmsi, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Own Ship MMSI"));
    resize(400, 280);

    const theme::MenuPalette& t = theme::menu();
    const theme::InputPalette& in = theme::input();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Own Ship MMSI"));
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("MMSI (9 digits)")));

    auto* body = new QWidget;
    auto* col = new QVBoxLayout(body);
    col->setContentsMargins(16, 4, 16, 12);
    col->setSpacing(10);

    auto* intro = new QLabel(QStringLiteral(
        "Enter the 9-digit MMSI of your vessel. Leave blank to clear."));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(t.actionFg));
    col->addWidget(intro);

    edit_ = new QLineEdit;
    edit_->setPlaceholderText(QStringLiteral("e.g. 123456789"));
    edit_->setMaxLength(9);
    edit_->setMinimumHeight(44);
    edit_->setStyleSheet(QStringLiteral(
        "QLineEdit{ font-size:16px; padding:6px 10px; background:%1; color:%2;"
        " border:1px solid %3; border-radius:6px; }"
        "QLineEdit:focus{ border:1px solid %4; }")
        .arg(in.fieldBg, in.fg, in.border, t.accent));
    // Allow only digits; length enforced by maxLength + updateOkState.
    edit_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("\\d{0,9}")), edit_));
    edit_->setText(currentMmsi);
    col->addWidget(edit_);
    panelCol->addWidget(body);

    panelCol->addStretch(1);
    panelCol->addWidget(dialogchrome::okCancelRow(this, &okBtn_));

    updateOkState();
    connect(edit_, &QLineEdit::textChanged, this, [this] { updateOkState(); });
}

QString OwnshipMmsiDialog::mmsi() const {
    return edit_->text().trimmed();
}

void OwnshipMmsiDialog::updateOkState() {
    const QString t = edit_->text().trimmed();
    okBtn_->setEnabled(t.isEmpty() || t.length() == 9);
}
