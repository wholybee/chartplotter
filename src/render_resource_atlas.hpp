#pragma once
// src/render_resource_atlas.hpp
//
// Stage 4: render resources + QPainter draw helpers (renderer architecture
// plan: RenderResourceAtlas).
//
// Owns the drawable symbology resources baked into symbols.bin / the
// rastersymbols-*.png sprite sheet: the point-symbol atlas (rects + pivots +
// name index), the compiled HPGL line-complex (LC) motifs, and the area-pattern
// (AP) tiles. Provides the device-space drawing operations the renderer calls
// (draw / drawLineComplex / fillAreaPattern) and the name→index lookups the
// portrayal engine needs to reference a symbol/LC/AP by name.
//
// This is the renderer-facing half of the old SymAtlas. It knows nothing about
// S-52 lookup rules or feature attributes — that lives in portrayal_engine.
//
// Thread safety: load() runs once on the GUI thread. After that the data is
// immutable; findSymbol / lineComplexIndex / areaPatternIndex are safe to call
// from worker threads, while the draw helpers run on the GUI thread.

#include <QPixmap>
#include <QImage>
#include <QRect>
#include <QRectF>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QPainterPath>
#include <QColor>
#include <QHash>
#include <QByteArray>
#include <QPainter>
#include <vector>
#include <cstddef>
#include <cstdint>

#include "portrayal_ir.hpp"      // kNoSymbol
#include "portrayal_binary.hpp"  // Bin* records

class RenderResourceAtlas {
public:
    // Populate from the parsed binary sections plus the sprite-sheet pixmap.
    // `strBase`/`strBytes` is the shared string blob (HPGL bytes live there).
    bool load(const BinSymRecord* symRecs, uint32_t symCount,
              const BinLcDefRecord* lcRecs, uint32_t lcCount,
              const BinApDefRecord* apRecs, uint32_t apCount,
              const char* strBase, std::size_t strBytes,
              const QPixmap& pm);

    bool isLoaded() const { return !atlas_.isNull(); }

    // Resolve a symbol name (e.g. "BOYPIL61") to its atlas index, or kNoSymbol.
    uint16_t findSymbol(const QByteArray& name) const;

    // Resolve an LC line-complex / AP area-pattern name to its def index, or -1.
    int lineComplexIndex(const QByteArray& name) const { return lcIndex_.value(name, -1); }
    int areaPatternIndex(const QByteArray& name) const { return apIndex_.value(name, -1); }

    bool hasLineComplex(int i) const { return i >= 0 && i < int(lcDefs_.size()); }
    bool hasAreaPattern(int i) const { return i >= 0 && i < int(apDefs_.size()); }

    // Draw symbol symIdx at screen point d, honouring the pivot offset.
    // rotationDeg rotates around the pivot (degrees CW from north). scale
    // multiplies the on-screen size around the pivot; 1.0 is the baked size.
    void draw(QPainter& p, uint16_t symIdx, QPointF d,
              float rotationDeg = 0.0f, float scale = 1.0f) const;

    // Device-space bounding box of symbol symIdx drawn at point d with `scale`,
    // ignoring rotation. Used as a keep-out box for label de-confliction; an
    // empty rect is returned for an unknown index. The geometry mirrors draw():
    // the pivot rides on d, so the box is offset by the scaled pivot.
    QRectF symbolBox(uint16_t symIdx, QPointF d, float scale = 1.0f) const {
        if (symIdx >= static_cast<uint16_t>(rects_.size())) return {};
        const QRect&  src = rects_[symIdx];
        const QPoint& piv = pivots_[symIdx];
        return QRectF(d.x() - piv.x() * scale, d.y() - piv.y() * scale,
                      src.width() * scale, src.height() * scale);
    }

    // Stamp LC line-complex `lcIndex` repeatedly along a device-space polyline,
    // each motif rotated to the local tangent. scale matches symbol scaling.
    void drawLineComplex(QPainter& p, int lcIndex,
                         const QPolygonF& devicePts, float scale) const;

    // Fill `deviceClipPath` (device coords) with AP pattern `apIndex`, tiled and
    // anchored at `anchor` (the device position of a fixed scene point) so the
    // pattern stays put under panning. scale matches symbol scaling.
    void fillAreaPattern(QPainter& p, int apIndex,
                         const QPainterPath& deviceClipPath,
                         QPointF anchor, float scale) const;

private:
    // A compiled HPGL motif (line-style or pattern), in HPGL units.
    struct HpglStroke { QPainterPath path; double width = 1.0; bool fill = false; };
    struct LcDef {
        std::vector<HpglStroke> strokes;   // HPGL units
        QColor   color;
        double   advance = 0.0;            // repeat length (HPGL units)
        QPointF  pivot;                    // HPGL pivot (rides the line)
        double   reach = 0.0;              // motif extent right of pivot (HPGL units)
    };
    struct ApDef {
        QImage   tile;          // pre-rendered motif (RGBA, nominal scale)
        QPointF  tileOrigin;    // px offset of the motif's origin within tile
        double   spacing = 8.0; // px between motif anchors (nominal scale)
        bool     staggered = false;
    };

    std::vector<HpglStroke> compileHpgl(const QByteArray& hpgl) const;

    QPixmap atlas_;
    std::vector<QRect>  rects_;
    std::vector<QPoint> pivots_;
    QHash<QByteArray, uint16_t> nameIndex_;

    std::vector<LcDef>     lcDefs_;
    QHash<QByteArray, int> lcIndex_;
    std::vector<ApDef>     apDefs_;
    QHash<QByteArray, int> apIndex_;
};
