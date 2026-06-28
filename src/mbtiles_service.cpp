#include "mbtiles_service.hpp"

#include <QDirIterator>
#include <QFileInfo>

MbtilesService::MbtilesService(QObject* parent) : QObject(parent) {
    // Register the value types carried across the queued worker→GUI signals.
    qRegisterMetaType<MbtilesMeta>("MbtilesMeta");
    qRegisterMetaType<QVector<MbtilesMeta>>("QVector<MbtilesMeta>");
}

MbtilesService::~MbtilesService() = default;

void MbtilesService::setFolders(const QStringList& dirs, quint64 gen) {
    gen_ = gen;
    readers_.clear();

    // Gather *.mbtiles across every folder, then open in a stable order so the
    // chartId (index into the combined list) is deterministic.
    QStringList files;
    for (const QString& dir : dirs) {
        if (dir.isEmpty()) continue;
        QDirIterator it(dir, QStringList{QStringLiteral("*.mbtiles")},
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) files << it.next();
    }
    files.removeDuplicates();
    files.sort();

    QVector<MbtilesMeta> charts;
    for (const QString& path : files) {
        auto reader = std::make_unique<MbtilesReader>();
        QString err;
        if (!reader->open(path, err)) continue;   // unreadable: skip silently

        if (!reader->meta().isRaster()) {
            emit message(QStringLiteral("Skipped vector MBTiles (unsupported): %1")
                             .arg(QFileInfo(path).fileName()));
            continue;
        }
        charts.push_back(reader->meta());
        readers_.push_back(std::move(reader));     // index == chartId
    }

    emit discovered(charts, gen);
}

void MbtilesService::requestTile(int chartId, int z, int x, int y, quint64 gen) {
    if (gen != gen_) return;                                   // stale folder
    if (chartId < 0 || chartId >= static_cast<int>(readers_.size())) return;

    const QByteArray blob = readers_[chartId]->tile(z, x, y);
    QImage img;
    if (!blob.isEmpty()) img.loadFromData(blob);   // null on absent / decode fail
    emit tileReady(chartId, z, x, y, img, gen);
}
