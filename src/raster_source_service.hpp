#pragma once
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
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
//
// A second, finer token — the request epoch — cancels in-flight work *within* a
// folder when the view moves. Plugin tiles can be slow (a big scanned raster with
// no pre-baked overviews decodes in tens of milliseconds), so a spell of zooming
// and panning can leave a long FIFO backlog of requests for views the user has
// already left; without cancellation the worker grinds through all of them before
// reaching the tiles now on screen, which is what makes a zoom-in take "minutes".
// ChartView stamps each request with the epoch current when it issued it, and
// raises setRequestFloor() as the view changes; a request whose epoch is below the
// floor when it reaches the head of the queue is skipped (reported via
// tileDropped, not tileReady) instead of decoded.
class RasterSourceService : public QObject {
    Q_OBJECT
public:
    explicit RasterSourceService(QObject* parent = nullptr);
    ~RasterSourceService() override;

    // Discard queued tile requests older than `epoch`. Deliberately a plain,
    // thread-safe atomic store, called *directly* from the GUI thread rather than
    // through a queued slot: the whole point is for the new floor to overtake the
    // request backlog it cancels, so it must not wait behind that same queue. The
    // worker reads it with acquire ordering at the top of requestTile().
    void setRequestFloor(quint64 epoch) noexcept {
        reqFloor_.store(epoch, std::memory_order_release);
    }

public slots:
    // Offer `dirs` to each source in `srcs` (a GUI-thread snapshot of the
    // registry) and catalog the ones that claim them. Unlike the vector side,
    // every source that canHandle() a folder contributes: the results are
    // concatenated in source order. Emits discovered(). `gen` becomes the
    // current generation; later tile requests must match it.
    void setSources(const QVector<IRasterChartSource*>& srcs,
                    const QStringList& dirs, quint64 gen);

    // Render one XYZ tile. Replies via tileReady() (null QImage = the tile is
    // empty or failed). Ignored if `gen` is stale (a newer folder). If `epoch` is
    // below the current request floor (a newer view superseded it) the tile is not
    // decoded at all — it replies via tileDropped() so the caller can free its
    // in-flight slot and re-request only if the tile is still on screen.
    void requestTile(int chartId, int z, int x, int y, quint64 gen, quint64 epoch);

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
    // A request skipped as stale (its view was superseded before it was rendered).
    // Carries no image: the caller only needs to release its in-flight slot. Kept
    // distinct from a null tileReady so a dropped tile is not cached as absent —
    // it may still be on screen and want re-requesting under the current epoch.
    void tileDropped(int chartId, int z, int x, int y, quint64 gen);

private:
    // Discovered charts, parallel to the ids handed out: entry i is chartId i.
    struct Entry {
        RasterSourceChart   chart;
        IRasterChartSource* source = nullptr;   // not owned (plugin owns it)
    };
    QVector<Entry> charts_;
    quint64        gen_ = 0;
    // Requests stamped below this are skipped without decoding. Written from the
    // GUI thread (setRequestFloor), read from the worker thread (requestTile).
    std::atomic<quint64> reqFloor_{0};
};
