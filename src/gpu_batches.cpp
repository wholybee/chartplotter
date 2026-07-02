// src/gpu_batches.cpp
#include "gpu_batches.hpp"

#include <QColor>

#include "cell_builder.hpp"   // cellbuilder::fillColor

namespace gpubatches {
namespace {

struct Rgb { float r, g, b; };

Rgb toRgb(const QColor& c) {
    return { static_cast<float>(c.redF()),
             static_cast<float>(c.greenF()),
             static_cast<float>(c.blueF()) };
}

// The outline pen for a feature, mirroring cellbuilder::instantiateCell's pen
// decision: the portrayal LS() colour when present, else the painter's per-kind
// fallback. Returns false when the feature draws no outline (bare DepthArea, or
// a fill-only OtherArea wash).
bool outlineColor(const Feature& f, const SymHit* hit, Rgb& out) {
    if (hit && hit->hasLine) {
        out = { hit->line.r / 255.f, hit->line.g / 255.f, hit->line.b / 255.f };
        return true;
    }
    switch (f.kind) {
        case FeatureKind::LandArea:     out = toRgb(QColor(115,  97,  64)); return true;
        case FeatureKind::Coastline:    out = toRgb(QColor( 64,  51,  31)); return true;
        case FeatureKind::DepthContour: out = toRgb(QColor(115, 153, 199)); return true;
        case FeatureKind::OtherArea:
            // Fill-only wash (AC without a line) draws no outline, matching the
            // painter's "fill-only area" branch.
            if (hit && hit->hasFill && hit->apIndex < 0) return false;
            out = toRgb(QColor(102, 102, 115));
            return true;
        case FeatureKind::OtherLine:    out = toRgb(QColor(102, 102, 128)); return true;
        case FeatureKind::DepthArea:    return false;   // bare depth area: no outline
        default:                        return false;   // soundings / points: no line here
    }
}

} // namespace

void appendCellBatches(const std::vector<Feature>& feats,
                       const PreparedCellRender& prep,
                       double originX, double originY,
                       std::vector<GpuVertex>& tris,
                       std::vector<GpuVertex>& lines) {
    auto hitFor = [&](std::size_t i) -> const SymHit* {
        return (i < prep.hasHit.size() && prep.hasHit[i]) ? &prep.hits[i] : nullptr;
    };

    // --- Area fills: the Stage 5 pre-triangulated batches ---------------------
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
        for (quint32 idx : pf.indices) {
            const std::size_t xi = static_cast<std::size_t>(idx) * 2;
            if (xi + 1 >= v.size()) continue;   // guard against a malformed fill
            tris.push_back({ static_cast<float>(v[xi]     - originX),
                             static_cast<float>(v[xi + 1] - originY),
                             col.r, col.g, col.b });
        }
    }

    // --- Outlines / lines -----------------------------------------------------
    for (std::size_t i = 0; i < feats.size(); ++i) {
        const Feature& f = feats[i];
        Rgb lc;
        if (!outlineColor(f, hitFor(i), lc)) continue;

        const bool closed = (f.kind == FeatureKind::DepthArea ||
                             f.kind == FeatureKind::LandArea ||
                             f.kind == FeatureKind::OtherArea);
        for (const auto& ring : f.rings) {
            const std::size_t n = ring.size();
            if (n < 2) continue;
            const std::size_t segs = closed ? n : n - 1;
            for (std::size_t s = 0; s < segs; ++s) {
                const Pt& a = ring[s];
                const Pt& b = ring[(s + 1) % n];
                lines.push_back({ static_cast<float>(a.x - originX),
                                  static_cast<float>(a.y - originY), lc.r, lc.g, lc.b });
                lines.push_back({ static_cast<float>(b.x - originX),
                                  static_cast<float>(b.y - originY), lc.r, lc.g, lc.b });
            }
        }
    }
}

} // namespace gpubatches
