#pragma once
// src/gpu_batches.hpp
//
// Stage 7: turn a built cell (Stage 7 A3) plus its prepared render (Stage 5)
// into retained GPU vertex batches for the QRhiWidget backend.
//
// Area fills are normally triangulated from the BuiltCell's already clipped and
// simplified BuiltPath geometry, so the GPU draws the painter's vertex budget
// instead of raw full-resolution source rings. Line segments come from the same
// BuiltPath geometry. Depth contours land in their own bucket so the show/hide
// toggle is a draw-list decision, not a rebuild.
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
// projected metres relative to (originX, originY), the origin-relative
// convention the QRhiWidget camera expects. `prep` supplies portrayal hits and
// may also contain legacy pre-triangulated fills; current builds append normal
// fills with appendBuiltCellFills() before calling this function. `cell`
// supplies the clipped/simplified painter geometry (its BuiltPath set).
void appendCellBatches(const std::vector<Feature>& feats,
                       const PreparedCellRender& prep,
                       const BuiltCell& cell,
                       double originX, double originY,
                       std::vector<GpuVertex>& tris,
                       std::vector<GpuVertex>& lines,
                       std::vector<GpuVertex>& contourLines);

// Append filled BuiltPath geometry from an already-instantiated cell. Used by
// GPU builds after clipping/simplification, avoiding full raw-cell
// triangulation during prepared-render compilation. Vertices are projected-
// frame metres relative to (originX, originY), same as appendCellBatches().
//
// Each fill is area-validated: if triangulating the simplified path doesn't
// cover the ring's area (the signature of a self-intersecting/collapsed ring the
// simplifier can still rarely produce), that ring's fill is dropped rather than
// substituted. A convex-hull substitute would flood a concave ring's whole
// extent (a coastline, or a basemap land/ocean polygon clipped to the view) with
// the fill colour — the cell-/view-spanning "giant triangle" artifact — so
// dropping the fill (its outline still strokes; the layer beneath shows through)
// is the safe choice. It never re-triangulates full-resolution rings either, so
// a bad ring can't reintroduce the raw-cell tessellation hotspot.
void appendBuiltCellFills(const BuiltCell& cell,
                          double originX, double originY,
                          std::vector<GpuVertex>& tris);

} // namespace gpubatches
