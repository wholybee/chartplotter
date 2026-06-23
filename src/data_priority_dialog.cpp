#include "data_priority_dialog.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

namespace { constexpr int kIdRole = Qt::UserRole; }

DataPriorityDialog::DataPriorityDialog(const QList<DataSourceInfo>& orderedSources, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Data Priority"));
    resize(440, 420);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Data Priority"));

    auto* body = new QWidget;
    auto* col = new QVBoxLayout(body);
    col->setContentsMargins(16, 10, 16, 8);
    col->setSpacing(8);

    auto* intro = new QLabel(QStringLiteral(
        "Order the navigation sources by priority. Data is used from the source "
        "highest in the list; when its data goes invalid, the next source takes "
        "over. Use Up / Down to reorder."));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(t.actionFg));
    col->addWidget(intro);

    auto* row = new QHBoxLayout;
    list_ = new QListWidget;
    list_->setStyleSheet(QStringLiteral(
        "QListWidget{ background:%1; color:%2; border:1px solid %3; border-radius:6px;"
        " outline:none; }"
        "QListWidget::item{ padding:12px; }"
        "QListWidget::item:selected{ background:%4; color:%5; }")
        .arg(t.panelBg, t.actionFg, t.separator, t.accent, t.titleFg));
    for (const DataSourceInfo& s : orderedSources) {
        auto* item = new QListWidgetItem(s.name, list_);
        item->setData(kIdRole, s.id);
    }
    if (list_->count() > 0) list_->setCurrentRow(0);
    row->addWidget(list_, 1);

    // Vertical Up/Down stack beside the list, sized for touch.
    auto* btns = new QVBoxLayout;
    auto* upBtn   = new QPushButton(QStringLiteral("▲  Up"));
    auto* downBtn = new QPushButton(QStringLiteral("▼  Down"));
    dialogchrome::styleOutlinedButton(upBtn);
    dialogchrome::styleOutlinedButton(downBtn);
    for (QPushButton* b : {upBtn, downBtn}) b->setMinimumHeight(48);
    btns->addWidget(upBtn);
    btns->addWidget(downBtn);
    btns->addStretch(1);
    row->addLayout(btns);
    col->addLayout(row, 1);
    panelCol->addWidget(body, 1);

    panelCol->addWidget(dialogchrome::okCancelRow(this));

    connect(upBtn,   &QPushButton::clicked, this, [this] { move(-1); });
    connect(downBtn, &QPushButton::clicked, this, [this] { move(+1); });
}

void DataPriorityDialog::move(int delta) {
    const int r = list_->currentRow();
    const int t = r + delta;
    if (r < 0 || t < 0 || t >= list_->count()) return;
    QListWidgetItem* item = list_->takeItem(r);
    list_->insertItem(t, item);
    list_->setCurrentRow(t);
}

QStringList DataPriorityDialog::orderedIds() const {
    QStringList ids;
    for (int i = 0; i < list_->count(); ++i)
        ids << list_->item(i)->data(kIdRole).toString();
    return ids;
}
