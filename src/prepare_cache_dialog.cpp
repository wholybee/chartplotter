#include "prepare_cache_dialog.hpp"
#include "prepared_chart_cache.hpp"
#include "chart_loader.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QWidget>
#include <QtConcurrent/QtConcurrentMap>
#include <algorithm>
#include <string>
#include <vector>

PrepareCacheDialog::PrepareCacheDialog(QStringList cellPaths, QWidget* parent)
    : QDialog(parent), paths_(std::move(cellPaths)) {
    setWindowTitle(QStringLiteral("Prepare Chart Cache"));
    resize(460, 280);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Prepare Chart Cache"));
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Parsed-cell cache")));

    auto* body = new QWidget;
    auto* col = new QVBoxLayout(body);
    col->setContentsMargins(16, 4, 16, 12);
    col->setSpacing(12);

    auto* intro = new QLabel(QStringLiteral(
        "Parsing %1 chart cell(s) and caching the result to disk. Future views "
        "of these cells skip the slow parse and load the prepared data directly. "
        "Cells already prepared are skipped. You can keep using the chart while "
        "this runs.").arg(paths_.size()));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(t.actionFg));
    col->addWidget(intro);

    bar_ = new QProgressBar;
    bar_->setRange(0, std::max(1, static_cast<int>(paths_.size())));
    bar_->setValue(0);
    bar_->setTextVisible(true);
    bar_->setMinimumHeight(26);
    bar_->setStyleSheet(QStringLiteral(
        "QProgressBar{ border:1px solid %1; border-radius:6px; background:%2;"
        " color:#ffffff; font-size:13px; text-align:center; }"
        "QProgressBar::chunk{ background:%3; border-radius:5px; }")
        .arg(t.separator, t.panelBg, t.accent));
    col->addWidget(bar_);

    status_ = new QLabel;
    status_->setStyleSheet(QStringLiteral(
        "font-size:13px; font-weight:600; color:%1;").arg(t.accent));
    col->addWidget(status_);

    panelCol->addWidget(body);
    panelCol->addStretch(1);

    // Single action button: cancels while running, closes when finished.
    auto* bar = new QWidget;
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(16, 8, 16, 16);
    row->setSpacing(10);
    actionBtn_ = new QPushButton(QStringLiteral("Cancel"), bar);
    dialogchrome::styleOutlinedButton(actionBtn_);
    row->addStretch(1);
    row->addWidget(actionBtn_);
    panelCol->addWidget(bar);

    connect(actionBtn_, &QPushButton::clicked, this, [this] {
        if (running_) future_.cancel();   // onFinished() turns the button into Close
        else          accept();
    });

    connect(&watcher_, &QFutureWatcher<void>::progressValueChanged,
            this, &PrepareCacheDialog::onProgress);
    connect(&watcher_, &QFutureWatcher<void>::finished,
            this, &PrepareCacheDialog::onFinished);

    updateStatus();
    start();
}

PrepareCacheDialog::~PrepareCacheDialog() {
    // Stop scheduling new cells and let in-flight parses drain before the pool
    // (and the atomics they write) are destroyed.
    if (running_) {
        future_.cancel();
        future_.waitForFinished();
    }
    pool_.waitForDone();
}

void PrepareCacheDialog::start() {
    if (paths_.isEmpty()) { onFinished(); return; }

    pool_.setMaxThreadCount(std::max(1, QThread::idealThreadCount() - 1));
    running_ = true;

    future_ = QtConcurrent::map(&pool_, paths_, [this](QString& path) {
        if (prepared_cache::isFresh(path)) {
            skipped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::vector<Feature> feats;
        BBox bbox;
        std::string err;
        if (chart::loadCellFeatures(path.toStdString(), feats, bbox, err) &&
            prepared_cache::store(path, feats, bbox)) {
            prepared_.fetch_add(1, std::memory_order_relaxed);
        } else {
            failed_.fetch_add(1, std::memory_order_relaxed);
        }
    });
    watcher_.setFuture(future_);
}

void PrepareCacheDialog::onProgress(int value) {
    bar_->setValue(value);
    updateStatus();
}

void PrepareCacheDialog::onFinished() {
    running_ = false;
    bar_->setValue(bar_->maximum());
    updateStatus();

    actionBtn_->setText(QStringLiteral("Close"));
    dialogchrome::styleAccentButton(actionBtn_);
}

void PrepareCacheDialog::updateStatus() {
    const int prepared = prepared_.load(std::memory_order_relaxed);
    const int skipped  = skipped_.load(std::memory_order_relaxed);
    const int failed   = failed_.load(std::memory_order_relaxed);
    const int done     = prepared + skipped + failed;

    QString s;
    if (!running_) {
        if (future_.isCanceled())
            s = QStringLiteral("Cancelled — ");
        else
            s = QStringLiteral("Done — ");
    }
    s += QStringLiteral("prepared %1").arg(prepared);
    if (skipped) s += QStringLiteral(" · skipped %1").arg(skipped);
    if (failed)  s += QStringLiteral(" · failed %1").arg(failed);
    if (running_) s += QStringLiteral("  (%1 / %2)").arg(done).arg(paths_.size());
    status_->setText(s);
}
