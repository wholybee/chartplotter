#pragma once
// src/gpu_batches.hpp
//
// Stage 7 (path A): turn a prepared cell render (Stage 5) into retained GPU
// vertex batches for the QRhiWidget backend — realizing the plan's "upload
// Stage 5 batches to GPU". Area fills come straight from the pre-triangulated
// PreparedFill; their colours mirror the painter (cellbuilder::fillColor for
// base depth/land areas, the portrayal AC() wash otherwise) so the GPU output
// matches the painter. Outline line-segments are derived from feature ring
// geometry, coloured from the portrayal line style when present, else the
// painter's per-kind fallback pens.
//
// This is deliberately widget-free (no QRhi types), so it is unit-testable and
// callable from either the harness or the app. Full S-52 parity — complex
// lines, area patterns, symbols, text, line width/stipple — is a later step;
// this covers fills + simple outlines.

#include <vector>

#include "gpu_vertex.hpp"       // GpuVertex
#include "chart_loader.hpp"     // Feature
#include "prepared_render.hpp"  // PreparedCellRender / PreparedFill

namespace gpubatches {

// Append one cell's fill triangles (to `tris`, a triangle list) and outline
// segments (to `lines`, a line list of endpoint pairs), in scene metres relative
// to (originX, originY) — the origin-relative convention the QRhiWidget camera
// expects. `prep` supplies the pre-triangulated fills and portrayal hits; both
// must be index-aligned to `feats` (as scene::compileScene produces).
void appendCellBatches(const std::vector<Feature>& feats,
                       const PreparedCellRender& prep,
                       double originX, double originY,
                       std::vector<GpuVertex>& tris,
                       std::vector<GpuVertex>& lines);

} // namespace gpubatches
