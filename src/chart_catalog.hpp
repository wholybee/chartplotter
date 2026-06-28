#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <vector>
#include "chart_loader.hpp"

class IChartSource;

// One ENC cell discovered in the tree: where it is, how big its footprint is,
// and which usage band (navigational purpose) it belongs to.
struct CellRecord {
    QString path;
    int     band = 0;          // 1=overview .. 6=berthing; 0 = unknown
    BBox    bbox;              // projected (Mercator), north-up
    bool    extentValid = false;
    // Actual data-coverage outline (M_COVR exterior rings, projected Mercator).
    // Empty when the cell has no usable M_COVR — consumers then treat the whole
    // bbox as the coverage. Drives quilting (drawing only the finest band in any
    // region; coarser bands fill only the gaps).
    std::vector<std::vector<Pt>> coverage;
};

// Scans a directory tree for ENC base cells and resolves each cell's footprint
// (cheaply, via M_COVR), caching results to disk so re-scans are instant. The
// scan runs on a worker thread; results are delivered via finished().
class ChartCatalog : public QObject {
    Q_OBJECT
public:
    explicit ChartCatalog(QObject* parent = nullptr);

    // Select the backend used by the next scan. nullptr (default) uses the
    // built-in ENC/S-57 reader (enumerate *.000 + GDAL coverage). A non-null
    // IChartSource (e.g. a CM93 plugin) supplies cells via its catalog().
    // Set on the UI thread before startScan(); read by the scan worker.
    void setSource(IChartSource* src) { source_ = src; }

    // Begins an asynchronous scan of one or more chart-set roots, aggregating
    // their cells into a single catalog. Ignored if a scan is already running.
    void startScan(const QStringList& roots);

    bool isScanning() const { return scanning_.load(); }
    const std::vector<CellRecord>& cells() const { return cells_; }   // valid after finished(true)
    BBox bounds() const { return bounds_; }
    QStringList roots() const { return roots_; }

    // Parse the navigational-purpose digit from an ENC cell filename.
    static int bandFromPath(const QString& path);

signals:
    void progress(int done, int total);
    void finished(bool ok, const QString& message);

private:
    // worker-thread bodies. cachePaths[i] is the disk cache for roots[i].
    void runScan(QStringList roots, QStringList cachePaths);
    void runScanFromSource(QStringList roots);

    std::vector<CellRecord> cells_;
    BBox    bounds_;
    QStringList roots_;
    std::atomic<bool> scanning_{false};
    IChartSource* source_ = nullptr;   // null = built-in ENC reader
};
