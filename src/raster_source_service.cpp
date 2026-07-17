#include "raster_source_service.hpp"

#include <QDebug>

RasterSourceService::RasterSourceService(QObject* parent) : QObject(parent) {
    // Queued signal/slot delivery across the worker boundary needs these.
    qRegisterMetaType<RasterSourceChart>("RasterSourceChart");
    qRegisterMetaType<QVector<RasterSourceChart>>("QVector<RasterSourceChart>");
    qRegisterMetaType<QVector<IRasterChartSource*>>("QVector<IRasterChartSource*>");
}

RasterSourceService::~RasterSourceService() = default;

void RasterSourceService::setSources(const QVector<IRasterChartSource*>& srcs,
                                     const QStringList& dirs, quint64 gen) {
    gen_ = gen;
    charts_.clear();
    if (srcs.isEmpty() || dirs.isEmpty()) {
        emit discovered({}, gen);
        return;
    }

    for (IRasterChartSource* s : srcs) {
        if (!s) continue;
        for (const QString& dir : dirs) {
            if (!s->canHandle(dir)) continue;

            std::vector<RasterSourceChart> found;
            QString err;
            const bool ok = s->catalog(dir, found, err,
                                       [](int /*done*/, int /*total*/) {});
            if (!ok) {
                emit message(QStringLiteral("%1: %2")
                                 .arg(s->displayName(),
                                      err.isEmpty() ? QStringLiteral("catalog failed") : err));
                continue;
            }
            for (RasterSourceChart& c : found)
                charts_.push_back(Entry{std::move(c), s});
        }
    }

    QVector<RasterSourceChart> out;
    out.reserve(charts_.size());
    for (const Entry& e : charts_) out << e.chart;
    emit discovered(out, gen);
}

void RasterSourceService::requestTile(int chartId, int z, int x, int y, quint64 gen) {
    if (gen != gen_) return;                       // a newer folder superseded this
    if (chartId < 0 || chartId >= charts_.size()) return;

    const Entry& e = charts_[chartId];
    QImage img;
    QString err;
    if (!e.source->tile(e.chart.id, z, x, y, img, err)) {
        // A real failure (vs. a legitimately empty tile, which returns true with
        // a null image). Report once per tile; the null reply below makes the
        // host cache it as absent so this can't spam.
        qWarning() << "raster source" << e.source->sourceId() << "tile" << z << x << y
                   << "failed:" << err;
        img = QImage();
    }
    emit tileReady(chartId, z, x, y, img, gen);
}

void RasterSourceService::dropSource(IRasterChartSource* /*src*/) {
    // Reaching here at all means the worker is not inside any source's tile()
    // (this slot and tile rendering run on the same thread) — which is the whole
    // point of the caller's blocking connection.
    //
    // Drop *every* chart, not just the dead source's: chartId is an index into
    // charts_, so erasing a subset would silently renumber the survivors under
    // any tile request already queued behind this call. Clearing makes those
    // requests no-ops (chartId out of range) instead. ChartView re-issues
    // setSources() with a fresh generation immediately after, which repopulates
    // the list and resyncs the generation — so this is a momentary blank, not a
    // permanent one. (Deliberately NOT bumping gen_ here: ChartView owns the
    // generation counter, and a unilateral bump would desync the two forever.)
    charts_.clear();
}
