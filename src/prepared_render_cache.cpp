#include "prepared_render_cache.hpp"
#include "render_scene_compiler.hpp"   // scene::kPreparedRenderFormat

#include <QByteArray>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace prepared_render_cache {
namespace {

constexpr quint32 kMagic        = 0x50524331; // "PRC1"
// Bump when this file's serialization layout changes, or when the decoder /
// projection feeding the features changes in a way that alters portrayal but
// not the source file's size/mtime. (symbols.bin changes are already covered by
// the runtime portrayal fingerprint.)
constexpr quint32 kCacheVersion = 1;

// SymHit floats (rotations, sector bearings) and fill vertices are genuine
// floats; no doubles are serialized, so single precision is exact and compact.
constexpr int kStreamVersion = QDataStream::Qt_5_15;

QString cacheFileFor(const QString& sourcePath) {
    const QString abs = QFileInfo(sourcePath).absoluteFilePath();
    const QByteArray h =
        QCryptographicHash::hash(abs.toUtf8(), QCryptographicHash::Sha1).toHex();
    return cacheDir() + QLatin1Char('/') + QString::fromLatin1(h) +
           QStringLiteral(".prend");
}

void writeSymHit(QDataStream& s, const SymHit& h) {
    s << static_cast<quint32>(h.symbols.size());
    for (const SymStamp& st : h.symbols) s << st.symIdx << st.rotationDeg;

    s << quint8(h.hasLine ? 1 : 0)
      << h.line.pattern << h.line.width << h.line.r << h.line.g << h.line.b;
    s << quint8(h.hasFill ? 1 : 0)
      << h.fill.r << h.fill.g << h.fill.b << h.fill.a;
    s << qint32(h.lcIndex) << qint32(h.apIndex);

    s << static_cast<quint32>(h.texts.size());
    for (const SymText& t : h.texts) {
        s << t.text << t.hjust << t.vjust << t.space
          << qint32(t.xoffs) << qint32(t.yoffs)
          << t.r << t.g << t.b << t.pointSize;
    }

    s << static_cast<quint32>(h.sectors.size());
    for (const SymSector& sec : h.sectors)
        s << sec.startDeg << sec.endDeg << sec.rangeNm << sec.r << sec.g << sec.b;
}

void readSymHit(QDataStream& s, SymHit& h) {
    quint32 n = 0;
    s >> n;
    h.symbols.resize(n);
    for (SymStamp& st : h.symbols) s >> st.symIdx >> st.rotationDeg;

    quint8 hasLine = 0;
    s >> hasLine; h.hasLine = hasLine != 0;
    s >> h.line.pattern >> h.line.width >> h.line.r >> h.line.g >> h.line.b;
    quint8 hasFill = 0;
    s >> hasFill; h.hasFill = hasFill != 0;
    s >> h.fill.r >> h.fill.g >> h.fill.b >> h.fill.a;
    qint32 lc = 0, ap = 0;
    s >> lc >> ap; h.lcIndex = lc; h.apIndex = ap;

    s >> n;
    h.texts.resize(n);
    for (SymText& t : h.texts) {
        qint32 xo = 0, yo = 0;
        s >> t.text >> t.hjust >> t.vjust >> t.space >> xo >> yo
          >> t.r >> t.g >> t.b >> t.pointSize;
        t.xoffs = xo; t.yoffs = yo;
    }

    s >> n;
    h.sectors.resize(n);
    for (SymSector& sec : h.sectors)
        s >> sec.startDeg >> sec.endDeg >> sec.rangeNm >> sec.r >> sec.g >> sec.b;
}

void writePayload(QDataStream& s, const PreparedCellRender& prep) {
    s << prep.formatVersion << prep.cellId;

    s << static_cast<quint32>(prep.hits.size());
    for (std::size_t i = 0; i < prep.hits.size(); ++i) {
        const quint8 has = (i < prep.hasHit.size()) ? prep.hasHit[i] : 0;
        s << has;
        if (has) writeSymHit(s, prep.hits[i]);
    }

    s << static_cast<quint32>(prep.fills.size());
    for (const PreparedFill& pf : prep.fills) {
        s << pf.featureIndex;
        s << static_cast<quint32>(pf.verts.size());
        for (float v : pf.verts) s << v;
        s << static_cast<quint32>(pf.indices.size());
        for (quint32 idx : pf.indices) s << idx;
    }
}

bool readPayload(QDataStream& s, PreparedCellRender& prep) {
    s >> prep.formatVersion >> prep.cellId;

    quint32 nFeat = 0;
    s >> nFeat;
    if (s.status() != QDataStream::Ok) return false;
    prep.hits.assign(nFeat, SymHit{});
    prep.hasHit.assign(nFeat, 0);
    for (quint32 i = 0; i < nFeat; ++i) {
        quint8 has = 0;
        s >> has;
        prep.hasHit[i] = has;
        if (has) readSymHit(s, prep.hits[i]);
    }

    quint32 nFills = 0;
    s >> nFills;
    if (s.status() != QDataStream::Ok) return false;
    prep.fills.resize(nFills);
    for (quint32 i = 0; i < nFills; ++i) {
        PreparedFill& pf = prep.fills[i];
        s >> pf.featureIndex;
        quint32 nv = 0; s >> nv;
        pf.verts.resize(nv);
        for (quint32 j = 0; j < nv; ++j) s >> pf.verts[j];
        quint32 ni = 0; s >> ni;
        pf.indices.resize(ni);
        for (quint32 j = 0; j < ni; ++j) s >> pf.indices[j];
    }
    return s.status() == QDataStream::Ok;
}

bool validateHeader(QDataStream& s, const QFileInfo& srcInfo,
                    quint64 portrayalFingerprint) {
    quint32 magic = 0, cacheVer = 0, renderFmt = 0;
    quint64 fp = 0;
    s >> magic >> cacheVer >> renderFmt >> fp;
    if (s.status() != QDataStream::Ok || magic != kMagic ||
        cacheVer != kCacheVersion ||
        renderFmt != scene::kPreparedRenderFormat ||
        fp != portrayalFingerprint)
        return false;

    QString storedPath;
    qint64 storedSize = 0, storedMtime = 0;
    s >> storedPath >> storedSize >> storedMtime;
    if (s.status() != QDataStream::Ok) return false;

    return storedPath == srcInfo.absoluteFilePath() &&
           storedSize == srcInfo.size() &&
           storedMtime == srcInfo.lastModified().toMSecsSinceEpoch();
}

} // namespace

QString cacheDir() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
           QStringLiteral("/prepared-render");
}

bool load(const QString& sourcePath, quint64 portrayalFingerprint,
          PreparedCellRender& out) {
    const QFileInfo srcInfo(sourcePath);
    if (!srcInfo.exists()) return false;

    QFile f(cacheFileFor(sourcePath));
    if (!f.open(QIODevice::ReadOnly)) return false;

    QDataStream s(&f);
    s.setVersion(kStreamVersion);
    s.setFloatingPointPrecision(QDataStream::SinglePrecision);

    if (!validateHeader(s, srcInfo, portrayalFingerprint)) return false;

    PreparedCellRender prep;
    if (!readPayload(s, prep)) return false;
    out = std::move(prep);
    return true;
}

bool store(const QString& sourcePath, quint64 portrayalFingerprint,
           const PreparedCellRender& prep) {
    const QFileInfo srcInfo(sourcePath);
    if (!srcInfo.exists()) return false;

    QDir().mkpath(cacheDir());

    QSaveFile f(cacheFileFor(sourcePath));
    if (!f.open(QIODevice::WriteOnly)) return false;

    QDataStream s(&f);
    s.setVersion(kStreamVersion);
    s.setFloatingPointPrecision(QDataStream::SinglePrecision);

    s << kMagic << kCacheVersion << scene::kPreparedRenderFormat
      << quint64(portrayalFingerprint);
    s << srcInfo.absoluteFilePath()
      << static_cast<qint64>(srcInfo.size())
      << static_cast<qint64>(srcInfo.lastModified().toMSecsSinceEpoch());

    writePayload(s, prep);

    if (s.status() != QDataStream::Ok) {
        f.cancelWriting();
        return false;
    }
    return f.commit();
}

} // namespace prepared_render_cache
