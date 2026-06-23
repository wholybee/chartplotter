#include "heading_source_dialog.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QButtonGroup>
#include <QWidget>

HeadingSourceDialog::HeadingSourceDialog(HeadingSource current, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Heading Source"));
    resize(420, 300);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Heading Source"));
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Heading Source")));

    auto* body = new QWidget;
    auto* col = new QVBoxLayout(body);
    col->setContentsMargins(16, 4, 16, 12);
    col->setSpacing(8);

    auto* intro = new QLabel(QStringLiteral(
        "Choose what the ownship symbol points along. If the chosen source has "
        "no data, the other is used as a fallback."));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(t.actionFg));
    col->addWidget(intro);

    headingRadio_ = new QRadioButton(QStringLiteral("Heading (where the bow points)"));
    cogRadio_     = new QRadioButton(QStringLiteral("COG (course over ground)"));
    for (QRadioButton* r : {headingRadio_, cogRadio_}) {
        r->setMinimumHeight(40);
        r->setCursor(Qt::PointingHandCursor);
        r->setStyleSheet(QStringLiteral("font-size:15px; color:%1; spacing:8px;").arg(t.actionFg));
        col->addWidget(r);
    }
    // Mutually exclusive selection.
    auto* group = new QButtonGroup(this);
    group->addButton(headingRadio_);
    group->addButton(cogRadio_);

    (current == HeadingSource::Cog ? cogRadio_ : headingRadio_)->setChecked(true);
    panelCol->addWidget(body);

    panelCol->addStretch(1);
    panelCol->addWidget(dialogchrome::okCancelRow(this));
}

HeadingSource HeadingSourceDialog::source() const {
    return cogRadio_->isChecked() ? HeadingSource::Cog : HeadingSource::Heading;
}
