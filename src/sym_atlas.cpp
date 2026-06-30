// src/sym_atlas.cpp
#include "sym_atlas.hpp"
#include "portrayal_binary.hpp"

#include <QFile>
#include <QFileInfo>
#include <QPixmap>

#include <cstddef>

// Loads the prebaked symbols.bin (magic "SYM\x06") and the rastersymbols-*.png
// sprite sheet, validates the header, then splits the file's sections between
// the portrayal package (rules/conditions/colours/instructions) and the render
// resource atlas (symbols/line-complex/area-pattern + the pixmap). The binary
// layout lives in portrayal_binary.hpp and must match tools/gen_symbols.cpp.

bool SymAtlas::load(const QString& binPath, const QString& pngPath) {
    QPixmap pm(pngPath);
    if (pm.isNull()) return false;

    QFile f(binPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray raw = f.readAll();
    f.close();

    if (static_cast<std::size_t>(raw.size()) < sizeof(BinHeader)) return false;
    const auto* hdr = reinterpret_cast<const BinHeader*>(raw.constData());
    if (hdr->magic[0] != 'S' || hdr->magic[1] != 'Y' ||
        hdr->magic[2] != 'M' || hdr->magic[3] != '\x06')
        return false;

    const std::size_t symBytes   = std::size_t(hdr->symCount)   * sizeof(BinSymRecord);
    const std::size_t lupBytes   = std::size_t(hdr->lupCount)   * sizeof(BinLupRecord);
    const std::size_t condBytes  = std::size_t(hdr->condCount)  * sizeof(BinCondRecord);
    const std::size_t attrBytes  = std::size_t(hdr->attrCount)  * sizeof(BinAttrRecord);
    const std::size_t colorBytes = std::size_t(hdr->colorCount) * sizeof(BinColorRecord);
    const std::size_t lcBytes    = std::size_t(hdr->lcCount)    * sizeof(BinLcDefRecord);
    const std::size_t apBytes    = std::size_t(hdr->apCount)    * sizeof(BinApDefRecord);
    const std::size_t strBytes   = std::size_t(hdr->strBytes);
    if (static_cast<std::size_t>(raw.size()) <
            sizeof(BinHeader) + symBytes + lupBytes + condBytes + attrBytes +
            colorBytes + lcBytes + apBytes + strBytes)
        return false;

    const char* p = raw.constData() + sizeof(BinHeader);
    const auto* symRecs   = reinterpret_cast<const BinSymRecord*>(p);  p += symBytes;
    const auto* lupRecs   = reinterpret_cast<const BinLupRecord*>(p);  p += lupBytes;
    const auto* condRecs  = reinterpret_cast<const BinCondRecord*>(p); p += condBytes;
    const auto* attrRecs  = reinterpret_cast<const BinAttrRecord*>(p); p += attrBytes;
    const auto* colorRecs = reinterpret_cast<const BinColorRecord*>(p);p += colorBytes;
    const auto* lcRecs    = reinterpret_cast<const BinLcDefRecord*>(p);p += lcBytes;
    const auto* apRecs    = reinterpret_cast<const BinApDefRecord*>(p);p += apBytes;
    const char* strBase   = p;

    package_.load(lupRecs, hdr->lupCount, condRecs, hdr->condCount,
                  attrRecs, hdr->attrCount, colorRecs, hdr->colorCount,
                  strBase, strBytes);

    if (!resources_.load(symRecs, hdr->symCount, lcRecs, hdr->lcCount,
                         apRecs, hdr->apCount, strBase, strBytes, pm))
        return false;

    // Portrayal fingerprint: size + mtime of the loaded binary. Changes whenever
    // symbols.bin is rebuilt, so the prepared-render cache rebuilds with it.
    const QFileInfo bi(binPath);
    fingerprint_ = (static_cast<quint64>(bi.size()) << 32) ^
                   static_cast<quint64>(bi.lastModified().toMSecsSinceEpoch());
    return true;
}
