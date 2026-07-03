#pragma once
// src/prepared_render.hpp
//
// Stage 5: prepared render representation (renderer architecture plan, Layer 5).
//
// PreparedCellRender is the precomputed, serializable, renderer-neutral result
// of portraying one chart cell: the expensive S-52 evaluation (best-match
// lookup + instruction execution + CS procedures) is run once at prepare time
// and stored, so loading a cell does not re-run portrayal. It is the third cache
// level (after the catalog cache and the parsed-cell cache) and is invalidated
// independently when the portrayal package changes.
//
// The portrayal result is stored as one SymHit per feature, aligned by index to
// the cell's feature vector (std::vector<Feature>, as produced by the decoder /
// parsed-cell cache). The scene is *instantiated* into a view (clipped,
// simplified, turned into QPainterPath batches) by ChartView::instantiateCell,
// which reads these SymHits instead of calling the portrayal engine.
//
// Older prepared-render caches stored area fills as PreparedFill vertex/index
// buffers. Current builds leave `fills` empty and generate GPU fill triangles
// from the clipped/simplified BuiltCell paths instead, avoiding expensive raw
// cell triangulation during cache generation.

#include <QString>
#include <cstdint>
#include <vector>
#include "portrayal_ir.hpp"   // SymHit

// Legacy pre-triangulated area fill, retained for serialization compatibility
// and tests. Current PreparedCellRender instances normally leave this vector
// empty.
struct PreparedFill {
    quint32 featureIndex = 0;
    std::vector<float>   verts;     // x0,y0, x1,y1, ...
    std::vector<quint32> indices;   // triangle triples
};

struct PreparedCellRender {
    quint32 formatVersion = 0;
    QString cellId;

    // Aligned to the cell's feature vector: hits[i] is the portrayal result for
    // feature i (default-constructed where none was computed); hasHit[i] marks
    // which entries were actually evaluated (lets serialization skip the empties).
    std::vector<SymHit> hits;
    std::vector<quint8> hasHit;

    // Pre-triangulated area fills (GPU-ready; unused by the QPainter path).
    std::vector<PreparedFill> fills;

    std::size_t featureCount() const { return hits.size(); }
};
