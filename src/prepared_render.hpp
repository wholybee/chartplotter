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
// Area fills are additionally pre-triangulated into PreparedFill (vertex +
// index buffers) ready for the retained GPU backend (Stage 7). The current
// QPainter path does not consume PreparedFill — it fills the rings directly —
// so the triangulation cannot affect today's on-screen output.

#include <QString>
#include <cstdint>
#include <vector>
#include "portrayal_ir.hpp"   // SymHit

// A pre-triangulated area fill, ready for a GPU vertex/index draw. `verts` holds
// interleaved scene-space x,y pairs (Y still north-up-positive, as in Feature
// rings); `indices` are triangle triples into those vertices. `featureIndex` ties
// it back to the cell's feature vector for styling/picking.
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
