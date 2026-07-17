#pragma once
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include "raster_chart_source.hpp"   // IRasterChartSource, RasterSourceChart

// Asynchronous driver for plugin-supplied raster chart backends — the
// IRasterChartSource twin of MbtilesService.
//
// One instance lives on the same worker thread as MbtilesService (both are
// I/O + image decode, and only one raster backend is usually active, so they
// share a thread rather than fighting for cores). ChartView talks to it purely
// through queued signals/slots, so every catalog()/tile() call into plugin code
// happens off the GUI thread.
//
// The service never touches RasterChartSourceRegistry: ChartView snapshots the
// registered sources on the GUI thread and hands the snapshot in with the
// folders, so the registry stays single-threaded.
//
// Charts are identified by a small integer id = index into the discovered list.
// Every request/response carries a generation token: bumping it (a new folder)
// invalidates in-flight work without races, since stale replies are ignored.
class RasterSourceService : public QObject {
    Q_OBJECT
public:
    explicit RasterSourceService(QObject* parent = nullptr);
    ~RasterSourceService() override;

public slots:
    // Offer `dirs` to each source in `srcs` (a GUI-thread snapshot of the
    // registry) and catalog the ones that claim them. Unlike the vector side,
    // every source that canHandle() a folder contributes: the results are
    // concatenated in source order. Emits discovered(). `gen` becomes the
    // current generation; later tile requests must match it.
    void setSources(const QVector<IRasterChartSource*>& srcs,
                    const QStringList& dirs, quint64 gen);

    // Render one XYZ tile. Replies via tileReady() (null QImage = the tile is
    // empty or failed). Ignored if `gen` is stale.
    void requestTile(int chartId, int z, int x, int y, quint64 gen);

    // Forget `src` and everything it advertised. Called with a BLOCKING queued
    // connection from ChartView::onRasterChartSourceUnregistered, so that when it
    // returns this thread is provably not inside src->tile() and holds no pointer
    // to it — the plugin may then destroy the source.
    void dropSource(IRasterChartSource* src);

signals:
    void discovered(const QVector<RasterSourceChart>& charts, quint64 gen);
    // Non-fatal note (e.g. one source failed to catalog). Shown in the status bar.
    void message(const QString& text);
    void tileReady(int chartId, int z, int x, int y, const QImage& img, quint64 gen);

private:
    // Discovered charts, parallel to the ids handed out: entry i is chartId i.
    struct Entry {
        RasterSourceChart   chart;
        IRasterChartSource* source = nullptr;   // not owned (plugin owns it)
    };
    QVector<Entry> charts_;
    quint64        gen_ = 0;
};
