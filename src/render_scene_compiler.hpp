#pragma once
// src/render_scene_compiler.hpp
//
// Stage 5: render scene compiler (renderer architecture plan, Layer 5).
//
// Converts a cell's parsed features into a PreparedCellRender: runs the S-52
// portrayal engine once per feature. The result is view-independent and
// cacheable; ChartView::instantiateCell turns it into a view-specific BuiltCell
// (clip + simplify + QPainterPath) without re-running portrayal.

#include <QString>
#include <cstdint>
#include <vector>
#include "chart_loader.hpp"      // Feature
#include "prepared_render.hpp"

class SymAtlas;

namespace scene {

// Prepared-render format version. Bump on any change to PreparedCellRender's
// layout or to what compileScene produces, so cached artifacts are invalidated.
// v3: prepared renders no longer pre-triangulate raw area fills. GPU fill
// batches are generated from clipped/simplified BuiltCell paths instead.
constexpr quint32 kPreparedRenderFormat = 3;

// Portray a cell. The resolvable rules mirror ChartView::instantiateCell
// exactly, so the SymHits stored here are precisely the ones the view would
// otherwise compute.
PreparedCellRender compileScene(const QString& cellId,
                                const std::vector<Feature>& feats,
                                const SymAtlas* atlas);

} // namespace scene
