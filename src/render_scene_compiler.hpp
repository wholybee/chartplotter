#pragma once
// src/render_scene_compiler.hpp
//
// Stage 5: render scene compiler (renderer architecture plan, Layer 5).
//
// Converts a cell's parsed features into a PreparedCellRender: runs the S-52
// portrayal engine once per feature and pre-triangulates area fills. The result
// is view-independent and cacheable; ChartView::instantiateCell turns it into a
// view-specific BuiltCell (clip + simplify + QPainterPath) without re-running
// portrayal.

#include <QString>
#include <cstdint>
#include <vector>
#include "chart_loader.hpp"      // Feature
#include "prepared_render.hpp"

class SymAtlas;

namespace scene {

// Prepared-render format version. Bump on any change to PreparedCellRender's
// layout or to what compileScene produces, so cached artifacts are invalidated.
// v2: fills are hole-aware (earcut-style bridged polygons + detached outers).
constexpr quint32 kPreparedRenderFormat = 2;

// Portray a cell and pre-triangulate its area fills. `atlas` may be null (e.g.
// the GSHHG basemap, which carries no symbol-bearing features): then no SymHits
// are produced and fills are still triangulated. The resolvable rules mirror
// ChartView::instantiateCell exactly, so the SymHits stored here are precisely
// the ones the view would otherwise compute.
PreparedCellRender compileScene(const QString& cellId,
                                const std::vector<Feature>& feats,
                                const SymAtlas* atlas);

} // namespace scene
