#pragma once
// src/chart_quilt.hpp
//
// Pure quilt / cell-visibility selection, extracted from ChartView (Stage 7 A3)
// so it is shared by the painter backend and a future retained GPU backend,
// neither of which should re-implement this subtle logic.
//
// "Quilting" is the rule that in any region of the screen only the finest usage
// band present draws: a cell contributes only the part of its coverage that a
// finer band has not already claimed; fully covered cells are dropped, and
// partially covered cells get a clip path so their geometry/symbols draw only
// where they are the finest data. Coarser bands then fill the gaps, eliminating
// stacked-duplicate symbology.
//
// This is deliberately widget- and camera-free: it takes the view-derived boxes
// (already in the projected/real frame) and a wrap-offset functor, and returns
// which cells draw, at what clip, and which must be present. The logic is a
// verbatim lift of the old in-widget updateVisibleCells selection, so painter
// output is unchanged.

#include <QSet>
#include <QHash>
#include <QString>
#include <QPainterPath>
#include <functional>
#include <vector>

#include "chart_catalog.hpp"   // CellRecord
#include "chart_loader.hpp"    // BBox, Pt

namespace chartquilt {

// Usage bands the quilt reasons about. ENC uses 1..6 (filename digit); CM93 has
// 8 native scales and maps each to its own band so overlapping scales never
// share one (which would double-draw).
constexpr int kMaxBand = 8;

struct QuiltResult {
    int maxBand = 0;                                // finest band actually chosen
    QSet<QString> active;                           // cells that contribute pixels
    QHash<QString, QPainterPath> drawClip;          // partial cells' clip, cell frame
    QSet<QString> wanted;                           // active cells reaching wantedArea
};

// Decide the quilt for the given catalog `cells` and the camera-derived boxes
// (projected/real frame): `wantedArea` (tight, drives loading), `keepArea` (the
// wider kept region the clips are bounded to). `target` is the nominal band for
// the current zoom. `wrapOffsetFor(cellCenterX)` returns the whole-world shift
// that brings a cell nearest the view centre (0 except across the 180° seam).
//
// Pure and Qt-value-only: no widget, no camera state, safe to call from either
// backend. Output is identical to the old ChartView::updateVisibleCells.
QuiltResult computeQuilt(const std::vector<CellRecord>& cells,
                         const BBox& wantedArea, const BBox& keepArea,
                         int target,
                         const std::function<double(double)>& wrapOffsetFor);

} // namespace chartquilt
