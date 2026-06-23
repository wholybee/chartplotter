#include "chart_sets_dialog.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QListWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QDir>

ChartSetsDialog::ChartSetsDialog(const QVector<ChartSet>& sets, QWidget* parent)
    : QDialog(parent), sets_(sets) {
    setWindowTitle(QStringLiteral("Chart Sets"));
    resize(560, 460);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Chart Sets"));

    auto* body = new QWidget;
    auto* col = new QVBoxLayout(body);
    col->setContentsMargins(16, 10, 16, 8);
    col->setSpacing(8);

    auto* hint = new QLabel(
        QStringLiteral("Define the chart folders you want to switch between."));
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(t.actionFg));
    col->addWidget(hint);

    list_ = new QListWidget;
    list_->setStyleSheet(QStringLiteral(
        "QListWidget{ background:%1; color:%2; border:1px solid %3; border-radius:6px;"
        " outline:none; }"
        "QListWidget::item{ padding:10px; }"
        "QListWidget::item:selected{ background:%4; color:%5; }")
        .arg(t.panelBg, t.actionFg, t.separator, t.accent, t.titleFg));
    col->addWidget(list_, 1);
    panelCol->addWidget(body, 1);

    auto* btnBar = new QWidget;
    auto* row = new QHBoxLayout(btnBar);
    row->setContentsMargins(16, 0, 16, 16);
    row->setSpacing(8);
    auto* addBtn = new QPushButton(QStringLiteral("Add…"));
    auto* rmBtn  = new QPushButton(QStringLiteral("Remove"));
    auto* doneBtn = new QPushButton(QStringLiteral("Done"));
    dialogchrome::styleOutlinedButton(addBtn);
    dialogchrome::styleOutlinedButton(rmBtn);
    dialogchrome::styleAccentButton(doneBtn);
    doneBtn->setDefault(true);
    row->addWidget(addBtn);
    row->addWidget(rmBtn);
    row->addStretch(1);
    row->addWidget(doneBtn);
    panelCol->addWidget(btnBar);

    connect(addBtn,  &QPushButton::clicked, this, &ChartSetsDialog::addSet);
    connect(rmBtn,   &QPushButton::clicked, this, &ChartSetsDialog::removeSelected);
    connect(doneBtn, &QPushButton::clicked, this, &QDialog::accept);

    refreshList();
}

void ChartSetsDialog::refreshList() {
    list_->clear();
    for (const ChartSet& cs : sets_) {
        auto* item = new QListWidgetItem(cs.name + QStringLiteral("\n") + cs.directory, list_);
        item->setToolTip(cs.directory);
    }
}

void ChartSetsDialog::addSet() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Select Chart Folder"));
    if (dir.isEmpty()) return;

    bool ok = false;
    const QString defName = QDir(dir).dirName();
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Chart Set Name"),
        QStringLiteral("Name for this chart set:"),
        QLineEdit::Normal, defName, &ok);
    if (!ok) return;

    ChartSet cs;
    cs.directory = dir;
    cs.name = name.trimmed().isEmpty() ? defName : name.trimmed();
    sets_.push_back(cs);
    refreshList();
    list_->setCurrentRow(sets_.size() - 1);
}

void ChartSetsDialog::removeSelected() {
    const int r = list_->currentRow();
    if (r < 0 || r >= sets_.size()) return;
    sets_.removeAt(r);
    refreshList();
}
