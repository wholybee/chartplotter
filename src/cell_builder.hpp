#pragma once
// src/cell_builder.hpp
//
// Pure, view-dependent instantiation of a chart cell into ready-to-draw
// primitives (BuiltCell). Extracted from ChartView (Stage 7 A3) so it is shared
// by the ENC pipeline, the basemap builder, and a future ChartEngine / GPU
// backend, none of which should depend on the ChartView widget.
//
// Portrayal is NOT recomputed here: it is resolved once by scene::compileScene
// into the PreparedCellRender, whose SymHits are aligned by index to `feats`.
// This reads those hits, so output is identical to the old in-widget builder.

#include <QColor>
#include <QString>
#include <vector>

#include "built_cell.hpp"
#include "chart_loader.hpp"      // Feature, BBox
#include "prepared_render.hpp"   // PreparedCellRender

namespace cellbuilder {

// Base area fill colour for a feature (S-52 day palette): land, then depth-band
// shading by DRVAL. Shared so the GPU batch builder colours fills identically to
// the painter. Only meaningful for DepthArea/LandArea; other kinds get their
// fill from the portrayal SymHit instead.
QColor fillColor(const Feature& f);

// Clip + simplify one cell to `clipBox` (projected, real frame) with vertex-merge
// tolerance `tol`, building its vector primitives sorted by z. Pure and
// Qt-value-only: safe to run on a worker thread. `prep` may be empty (e.g. the
// basemap): a missing/default hit behaves exactly as "no symbol resolved".
BuiltCell instantiateCell(const QString& path, const std::vector<Feature>& feats,
                          const PreparedCellRender& prep,
                          int band, const BBox& clipBox, double tol);

} // namespace cellbuilder
