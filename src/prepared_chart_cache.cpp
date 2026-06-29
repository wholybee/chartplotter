#include "prepared_chart_cache.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace prepared_cache {
namespace {

// Magic + versions. Any layout change to the payload below requires bumping
// kFormatVersion; a change to chart::loadCellFeatures' output requires bumping
// kDecoderVersion; a change to projection.hpp requires bumping kProjVersion.
// A mismatch on any of these is treated as a cache miss, so stale files are
// simply re-parsed and overwritten — never trusted.
constexpr quint32 kMagic         = 0x50434331; // "PCC1"
constexpr quint32 kFormatVersion = 1;
constexpr quint32 kDecoderVersion = 1;
constexpr quint32 kProjVersion    = 1;

// QDataStream wire version pinned so a future Qt upgrade cannot silently change
// the encoding of the primitives we read back.
constexpr int kStreamVersion = QDataStream::Qt_5_15;

QString cacheFileFor(const QString& sourcePath) {
    // Hash the absolute path for a flat, filesystem-safe file name. The header
    // also stores the full path, so a (vanishingly unlikely) hash collision is
    // caught at load time and treated as a miss.
    const QString abs = QFileInfo(sourcePath).absoluteFilePath();
    const QByteArray h =
        QCryptographicHash::hash(abs.toUtf8(), QCryptographicHash::Sha1).toHex();
    return cacheDir() + QLatin1Char('/') + QString::fromLatin1(h) +
           QStringLiteral(".pcell");
}

void writeBBox(QDataStream& s, const BBox& b) {
    s << b.minx << b.miny << b.maxx << b.maxy;
}

void readBBox(QDataStream& s, BBox& b) {
    s >> b.minx >> b.miny >> b.maxx >> b.maxy;
}

void writeStdString(QDataStream& s, const std::string& str) {
    const quint32 n = static_cast<quint32>(str.size());
    s << n;
    if (n) s.writeRawData(str.data(), static_cast<int>(n));
}

bool readStdString(QDataStream& s, std::string& str) {
    quint32 n = 0;
    s >> n;
    str.resize(n);
    if (n && s.readRawData(&str[0], static_cast<int>(n)) != static_cast<int>(n))
        return false;
    return true;
}

void writeFeature(QDataStream& s, const Feature& f) {
    s << static_cast<qint32>(f.kind);
    s << static_cast<qint32>(f.zorder);

    s << static_cast<quint32>(f.rings.size());
    for (const std::vector<Pt>& ring : f.rings) {
        s << static_cast<quint32>(ring.size());
        for (const Pt& p : ring) s << p.x << p.y;
    }

    s << f.depth;
    s << static_cast<quint8>(f.hasDepth ? 1 : 0);
    s << static_cast<qint32>(f.scaleMin);
    writeBBox(s, f.bbox);
    writeStdString(s, f.objClass);

    s << static_cast<quint32>(f.attrs.size());
    for (const auto& a : f.attrs) {
        writeStdString(s, a.first);
        writeStdString(s, a.second);
    }

    writeStdString(s, f.name);
}

bool readFeature(QDataStream& s, Feature& f) {
    qint32 kind = 0, zorder = 0, scaleMin = 0;
    s >> kind;
    f.kind = static_cast<FeatureKind>(kind);
    s >> zorder;
    f.zorder = zorder;

    quint32 nRings = 0;
    s >> nRings;
    f.rings.resize(nRings);
    for (quint32 i = 0; i < nRings; ++i) {
        quint32 nPts = 0;
        s >> nPts;
        std::vector<Pt>& ring = f.rings[i];
        ring.resize(nPts);
        for (quint32 j = 0; j < nPts; ++j) s >> ring[j].x >> ring[j].y;
    }

    s >> f.depth;
    quint8 hasDepth = 0;
    s >> hasDepth;
    f.hasDepth = hasDepth != 0;
    s >> scaleMin;
    f.scaleMin = scaleMin;
    readBBox(s, f.bbox);
    if (!readStdString(s, f.objClass)) return false;

    quint32 nAttrs = 0;
    s >> nAttrs;
    f.attrs.resize(nAttrs);
    for (quint32 i = 0; i < nAttrs; ++i) {
        if (!readStdString(s, f.attrs[i].first)) return false;
        if (!readStdString(s, f.attrs[i].second)) return false;
    }

    if (!readStdString(s, f.name)) return false;
    return s.status() == QDataStream::Ok;
}

// Read and validate a cache file's header against the live source file. On
// success the stream is positioned at the start of the payload (bbox) and true
// is returned; any version, identity, or freshness mismatch returns false.
bool validateHeader(QDataStream& s, const QFileInfo& srcInfo) {
    quint32 magic = 0, format = 0, decoder = 0, projv = 0;
    s >> magic >> format >> decoder >> projv;
    if (s.status() != QDataStream::Ok || magic != kMagic ||
        format != kFormatVersion || decoder != kDecoderVersion ||
        projv != kProjVersion)
        return false;

    QString storedPath;
    qint64 storedSize = 0, storedMtime = 0;
    s >> storedPath >> storedSize >> storedMtime;
    if (s.status() != QDataStream::Ok) return false;

    // mtime is stored in ms since epoch (UTC) to avoid timezone drift.
    return storedPath == srcInfo.absoluteFilePath() &&
           storedSize == srcInfo.size() &&
           storedMtime == srcInfo.lastModified().toMSecsSinceEpoch();
}

} // namespace

QString cacheDir() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
           QStringLiteral("/parsed-cells");
}

bool isFresh(const QString& sourcePath) {
    const QFileInfo srcInfo(sourcePath);
    if (!srcInfo.exists()) return false;

    QFile f(cacheFileFor(sourcePath));
    if (!f.open(QIODevice::ReadOnly)) return false;

    QDataStream s(&f);
    s.setVersion(kStreamVersion);
    s.setFloatingPointPrecision(QDataStream::DoublePrecision);
    return validateHeader(s, srcInfo);
}

bool load(const QString& sourcePath, std::vector<Feature>& out, BBox& bbox) {
    const QFileInfo srcInfo(sourcePath);
    if (!srcInfo.exists()) return false;

    QFile f(cacheFileFor(sourcePath));
    if (!f.open(QIODevice::ReadOnly)) return false;

    QDataStream s(&f);
    s.setVersion(kStreamVersion);
    s.setFloatingPointPrecision(QDataStream::DoublePrecision);

    if (!validateHeader(s, srcInfo)) return false;

    readBBox(s, bbox);
    quint32 nFeats = 0;
    s >> nFeats;
    if (s.status() != QDataStream::Ok) return false;

    std::vector<Feature> feats;
    feats.resize(nFeats);
    for (quint32 i = 0; i < nFeats; ++i) {
        if (!readFeature(s, feats[i])) return false;
    }
    if (s.status() != QDataStream::Ok) return false;

    out = std::move(feats);
    return true;
}

bool store(const QString& sourcePath, const std::vector<Feature>& feats,
           const BBox& bbox) {
    const QFileInfo srcInfo(sourcePath);
    if (!srcInfo.exists()) return false;

    QDir().mkpath(cacheDir());

    // QSaveFile writes to a temp file and atomically renames on commit(), so a
    // concurrent reader never sees a partial cache file.
    QSaveFile f(cacheFileFor(sourcePath));
    if (!f.open(QIODevice::WriteOnly)) return false;

    QDataStream s(&f);
    s.setVersion(kStreamVersion);
    s.setFloatingPointPrecision(QDataStream::DoublePrecision);

    s << kMagic << kFormatVersion << kDecoderVersion << kProjVersion;
    s << srcInfo.absoluteFilePath()
      << static_cast<qint64>(srcInfo.size())
      << static_cast<qint64>(srcInfo.lastModified().toMSecsSinceEpoch());

    writeBBox(s, bbox);
    s << static_cast<quint32>(feats.size());
    for (const Feature& ft : feats) writeFeature(s, ft);

    if (s.status() != QDataStream::Ok) {
        f.cancelWriting();
        return false;
    }
    return f.commit();
}

} // namespace prepared_cache
