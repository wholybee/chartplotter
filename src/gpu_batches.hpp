#pragma once
// src/gpu_batches.hpp
//
// Stage 7: turn a built cell (Stage 7 A3) plus its prepared render (Stage 5)
// into retained GPU vertex batches for the QRhiWidget backend.
//
// Area fills come from the pre-triangulated PreparedFill batches, filtered to
// the cell's clip box so an oversized cell doesn't upload geometry far outside
// the kept region; their colours mirror the painter (cellbuilder::fillColor for
// base depth/land areas, the portrayal AC() wash otherwise). Line segments come
// from the BuiltCell's BuiltPath geometry — the same keep-area-clipped,
// per-band-simplified polylines the painter strokes — so the GPU draws the
// painter's vertex budget, not the raw full-resolution source rings. Depth
// contours land in their own bucket so the show/hide toggle is a draw-list
// decision, not a rebuild.
//
// This is deliberately widget-free (no QRhi types), so it is unit-testable and
// callable from either the harness or the app. Full S-52 parity — complex
// lines, area patterns, symbols, text, line width/stipple — is a later step;
// this covers fills + simple outlines.

#include <vector>

#include "gpu_vertex.hpp"       // GpuVertex
#include "chart_loader.hpp"     // Feature
#include "prepared_render.hpp"  // PreparedCellRender / PreparedFill
#include "built_cell.hpp"       // BuiltCell / BuiltPath

namespace gpubatches {

// Append one cell's fill triangles (to `tris`, a triangle list) and line
// segments (to `lines` / `contourLines`, line lists of endpoint pairs), in
// projected metres relative to (originX, originY) — the origin-relative
// convention the QRhiWidget camera expects. `prep` supplies the
// pre-triangulated fills and portrayal hits, index-aligned to `feats`; `cell`
// supplies the clipped/simplified painter geometry (its BuiltPath set) and the
// clip box used to filter the fills.
void appendCellBatches(const std::vector<Feature>& feats,
                       const PreparedCellRender& prep,
                       const BuiltCell& cell,
                       double originX, double originY,
                       std::vector<GpuVertex>& tris,
                       std::vector<GpuVertex>& lines,
                       std::vector<GpuVertex>& contourLines);

} // namespace gpubatches
