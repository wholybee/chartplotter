#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QImage>
#include <memory>
#include <vector>
#include "mbtiles_reader.hpp"

// Asynchronous MBTiles backend. One instance lives on a dedicated worker thread
// (moveToThread); the ChartView talks to it purely through queued signals/slots
// so all SQLite access and image decoding happen off the GUI thread.
//
// Charts are identified by a small integer id = index into the discovered list.
// Every request/response carries a generation token: bumping it (a new folder)
// invalidates in-flight work without races, since stale replies are ignored.
class MbtilesService : public QObject {
    Q_OBJECT
public:
    explicit MbtilesService(QObject* parent = nullptr);
    ~MbtilesService() override;

public slots:
    // Rescan one or more chart folders for *.mbtiles (recursive). Opens the
    // raster ones, emits discovered() with their combined metadata. `gen`
    // becomes the current generation; later tile requests must match it.
    void setFolders(const QStringList& dirs, quint64 gen);

    // Fetch + decode one XYZ tile. Replies via tileReady() (null QImage = the
    // tile is absent or failed to decode). Ignored if `gen` is stale.
    void requestTile(int chartId, int z, int x, int y, quint64 gen);

signals:
    void discovered(const QVector<MbtilesMeta>& charts, quint64 gen);
    // Non-fatal note (e.g. a vector .mbtiles was skipped). Shown in the status bar.
    void message(const QString& text);
    void tileReady(int chartId, int z, int x, int y, const QImage& img, quint64 gen);

private:
    std::vector<std::unique_ptr<MbtilesReader>> readers_;   // index == chartId
    quint64 gen_ = 0;
};
