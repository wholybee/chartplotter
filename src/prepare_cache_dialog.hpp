#pragma once
#include <QDialog>
#include <QFuture>
#include <QFutureWatcher>
#include <QStringList>
#include <QThreadPool>
#include <atomic>

class QLabel;
class QProgressBar;
class QPushButton;

// Stage 2 "Prepare chart cache" action (docs/renderer_implementation_stages.md).
//
// Runs the parsed-cell cache build for a set of ENC cells up front, so the
// first on-water view of each cell reads a prepared binary instead of paying a
// cold GDAL parse. Cells already cached and current are skipped cheaply
// (prepared_cache::isFresh), so re-running after adding a few charts only does
// the new work.
//
// The parse pass runs on a private QThreadPool (nCPU-1 workers) off the UI
// thread via QtConcurrent::map; the dialog stays responsive and can be
// cancelled. Only built-in ENC cells (real file paths) are handled — plugin
// chart sources with opaque cell ids aren't cached yet.
class PrepareCacheDialog : public QDialog {
    Q_OBJECT
public:
    // `cellPaths` are absolute ENC cell file paths to prepare. Preparation
    // starts automatically when the dialog is shown.
    explicit PrepareCacheDialog(QStringList cellPaths, QWidget* parent = nullptr);
    ~PrepareCacheDialog() override;

private:
    void start();
    void onProgress(int value);
    void onFinished();
    void updateStatus();

    QStringList   paths_;
    QThreadPool   pool_;                 // declared before the future/watcher
    QFuture<void> future_;
    QFutureWatcher<void> watcher_;

    // Result tallies, written on worker threads, read on the UI thread.
    std::atomic<int> prepared_{0};
    std::atomic<int> skipped_{0};
    std::atomic<int> failed_{0};

    QLabel*       status_   = nullptr;
    QProgressBar* bar_      = nullptr;
    QPushButton*  actionBtn_ = nullptr;  // "Cancel" while running, "Close" when done
    bool          running_  = false;
};
