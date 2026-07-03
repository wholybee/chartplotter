// src/gpu_batches.cpp
#include "gpu_batches.hpp"

#include <QColor>
#include <QPainterPath>

#include <algorithm>

#include "cell_builder.hpp"   // cellbuilder::fillColor

namespace gpubatches {
namespace {

struct Rgb { float r, g, b; };

Rgb toRgb(const QColor& c) {
    return { static_cast<float>(c.redF()),
             static_cast<float>(c.greenF()),
             static_cast<float>(c.blueF()) };
}

} // namespace

void appendCellBatches(const std::vector<Feature>& feats,
                       const PreparedCellRender& prep,
                       const BuiltCell& cell,
                       double originX, double originY,
                       std::vector<GpuVertex>& tris,
                       std::vector<GpuVertex>& lines,
                       std::vector<GpuVertex>& contourLines) {
    auto hitFor = [&](std::size_t i) -> const SymHit* {
        return (i < prep.hasHit.size() && prep.hasHit[i]) ? &prep.hits[i] : nullptr;
    };

    // --- Area fills: the Stage 5 pre-triangulated batches, filtered to the
    // cell's clip box. The triangles are whole-cell (the painter's clipped fill
    // geometry is QPainterPath, not triangles, so it can't be reused here);
    // instead cull per triangle by bounding-box overlap — that drops the bulk
    // of an oversized cell while keeping every triangle that could touch the
    // kept region (the GPU clips the survivors at the viewport).
    const BBox& clip = cell.clipBox;
    const bool doClip = clip.valid();
    for (const PreparedFill& pf : prep.fills) {
        if (pf.featureIndex >= feats.size()) continue;
        const Feature& f = feats[pf.featureIndex];
        const SymHit* hit = hitFor(pf.featureIndex);

        Rgb col;
        if (f.kind == FeatureKind::LandArea || f.kind == FeatureKind::DepthArea) {
            col = toRgb(cellbuilder::fillColor(f));
        } else if (hit && hit->hasFill) {
            col = { hit->fill.r / 255.f, hit->fill.g / 255.f, hit->fill.b / 255.f };
        } else {
            continue;   // AP-only area (pattern, no wash): painter draws no solid fill
        }

        const std::vector<float>& v = pf.verts;
        const std::size_t triCount = pf.indices.size() / 3;
        for (std::size_t t = 0; t < triCount; ++t) {
            const std::size_t x0 = static_cast<std::size_t>(pf.indices[3 * t])     * 2;
            const std::size_t x1 = static_cast<std::size_t>(pf.indices[3 * t + 1]) * 2;
            const std::size_t x2 = static_cast<std::size_t>(pf.indices[3 * t + 2]) * 2;
            if (x0 + 1 >= v.size() || x1 + 1 >= v.size() || x2 + 1 >= v.size())
                continue;   // guard against a malformed fill
            if (doClip) {
                const float minX = std::min({ v[x0], v[x1], v[x2] });
                const float maxX = std::max({ v[x0], v[x1], v[x2] });
                const float minY = std::min({ v[x0 + 1], v[x1 + 1], v[x2 + 1] });
                const float maxY = std::max({ v[x0 + 1], v[x1 + 1], v[x2 + 1] });
                if (maxX < clip.minx || minX > clip.maxx ||
                    maxY < clip.miny || minY > clip.maxy)
                    continue;   // triangle entirely outside the kept region
            }
            for (const std::size_t xi : { x0, x1, x2 })
                tris.push_back({ static_cast<float>(v[xi]     - originX),
                                 static_cast<float>(v[xi + 1] - originY),
                                 col.r, col.g, col.b });
        }
    }

    // --- Outlines / lines: from the BuiltCell's clipped + simplified BuiltPath
    // geometry — exactly the polylines the painter strokes, with its pen
    // colours. BuiltPath is in the scene frame (Y = -projected Y), so Y is
    // negated back to the projected frame the GPU batches use. The builder
    // emits only MoveTo/LineTo elements (closeSubpath appends the closing
    // LineTo), so element iteration reconstructs the segments directly.
    for (const BuiltPath& bp : cell.paths) {
        if (!bp.hasPen) continue;
        std::vector<GpuVertex>& out = bp.isDepthContour ? contourLines : lines;
        const Rgb lc = toRgb(bp.penColor);
        const int n = bp.path.elementCount();
        bool haveCur = false;
        float cx = 0.0f, cy = 0.0f;
        for (int i = 0; i < n; ++i) {
            const QPainterPath::Element& e = bp.path.elementAt(i);
            const float x = static_cast<float>(e.x - originX);
            const float y = static_cast<float>(-e.y - originY);   // scene -> projected
            if (e.type == QPainterPath::MoveToElement) {
                cx = x; cy = y; haveCur = true;
                continue;
            }
            if (e.type != QPainterPath::LineToElement) { haveCur = false; continue; }
            if (haveCur) {
                out.push_back({ cx, cy, lc.r, lc.g, lc.b });
                out.push_back({ x,  y,  lc.r, lc.g, lc.b });
            }
            cx = x; cy = y; haveCur = true;
        }
    }
}

} // namespace gpubatches
