// src/gpu_batches.cpp
#include "gpu_batches.hpp"

#include <QColor>
#include <QPainterPath>

#include <algorithm>
#include <limits>

#include "cell_builder.hpp"   // cellbuilder::fillColor
#include "geom_tessellate.hpp"

namespace gpubatches {
namespace {

struct Rgb { float r, g, b; };

Rgb toRgb(const QColor& c) {
    return { static_cast<float>(c.redF()),
             static_cast<float>(c.greenF()),
             static_cast<float>(c.blueF()) };
}

bool samePoint(const Pt& a, const Pt& b) {
    return a.x == b.x && a.y == b.y;
}

void cleanRing(std::vector<Pt>& ring) {
    if (ring.empty()) return;
    std::vector<Pt> clean;
    clean.reserve(ring.size());
    for (const Pt& p : ring) {
        if (clean.empty() || !samePoint(clean.back(), p))
            clean.push_back(p);
    }
    if (clean.size() > 1 && samePoint(clean.front(), clean.back()))
        clean.pop_back();
    ring.swap(clean);
}

// Triangulate one simple polygon (already hole-merged) and append its triangles.
// Returns false when the ear-clip result doesn't cover the ring's area — the
// signature of a self-intersecting or collapsed ring, which the ring-aware
// simplifier makes rare but can't fully rule out — so the caller can fall back
// to safer geometry (E). Nothing is appended on failure. A ring with < 3
// vertices or ~zero net area draws nothing and is reported as success.
bool appendTriangulatedRing(std::vector<Pt> ring, const Rgb& col,
                            double originX, double originY,
                            std::vector<GpuVertex>& tris) {
    cleanRing(ring);
    if (ring.size() < 3) return true;
    const std::vector<uint32_t> idx = geomtess::triangulate(ring);

    // Area check: a correct ear-clip of a simple polygon tiles it exactly, so
    // the emitted triangle areas sum to the ring's area. Overlapping "large
    // triangles" (self-intersection) inflate the sum; an early bail-out
    // deflates it. Either way the sums diverge and we reject the result.
    const double want = std::abs(geomtess::signedArea2(ring)) * 0.5;
    double got = 0.0;
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        const Pt& a = ring[idx[t]];
        const Pt& b = ring[idx[t + 1]];
        const Pt& c = ring[idx[t + 2]];
        got += std::abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) * 0.5;
    }
    constexpr double kAreaEps = 1e-6;
    const bool bad = (want <= kAreaEps) ? (got > kAreaEps)          // net-zero ring, spurious coverage
                                        : (std::abs(got - want) > 0.01 * want);
    if (bad) return false;

    tris.reserve(tris.size() + idx.size());
    for (uint32_t i : idx) {
        if (i >= ring.size()) continue;
        const Pt& p = ring[i];
        tris.push_back({ static_cast<float>(p.x - originX),
                         static_cast<float>(p.y - originY),
                         col.r, col.g, col.b });
    }
    return true;
}

struct RingInfo {
    std::vector<Pt> pts;
    double area = 0.0;
    int parent = -1;
    int depth = 0;
};

int ringDepth(const std::vector<RingInfo>& rings, int i) {
    int d = 0;
    for (int p = rings[i].parent; p >= 0; p = rings[p].parent) ++d;
    return d;
}

// Returns false (and appends nothing more) as soon as any ring fails its area
// check, so the caller can roll back and retry the whole path on full-resolution
// geometry. Returns true when every ring triangulated cleanly.
bool appendTriangulatedPathRings(std::vector<std::vector<Pt>> rings, const Rgb& col,
                                 double originX, double originY,
                                 std::vector<GpuVertex>& tris) {
    std::vector<RingInfo> clean;
    clean.reserve(rings.size());
    for (std::vector<Pt>& ring : rings) {
        cleanRing(ring);
        if (ring.size() < 3) continue;
        RingInfo ri;
        ri.area = std::abs(geomtess::signedArea2(ring));
        ri.pts = std::move(ring);
        clean.push_back(std::move(ri));
    }
    if (clean.empty()) return true;
    if (clean.size() == 1)
        return appendTriangulatedRing(std::move(clean[0].pts), col, originX, originY, tris);

    for (std::size_t i = 0; i < clean.size(); ++i) {
        double parentArea = std::numeric_limits<double>::max();
        for (std::size_t j = 0; j < clean.size(); ++j) {
            if (i == j || clean[j].area <= clean[i].area) continue;
            if (!geomtess::pointInRing(clean[i].pts[0], clean[j].pts)) continue;
            if (clean[j].area < parentArea) {
                parentArea = clean[j].area;
                clean[i].parent = static_cast<int>(j);
            }
        }
    }
    for (std::size_t i = 0; i < clean.size(); ++i)
        clean[i].depth = ringDepth(clean, static_cast<int>(i));

    for (std::size_t i = 0; i < clean.size(); ++i) {
        if ((clean[i].depth % 2) != 0) continue;   // odd-even hole
        std::vector<const std::vector<Pt>*> holes;
        for (std::size_t j = 0; j < clean.size(); ++j) {
            if (clean[j].parent == static_cast<int>(i) &&
                (clean[j].depth % 2) != 0)
                holes.push_back(&clean[j].pts);
        }
        const bool ok = holes.empty()
            ? appendTriangulatedRing(clean[i].pts, col, originX, originY, tris)
            : appendTriangulatedRing(geomtess::mergeHoles(clean[i].pts, std::move(holes)),
                                     col, originX, originY, tris);
        if (!ok) return false;
    }
    return true;
}

// Rebuild the ring set of a filled BuiltPath from its QPainterPath, mapping the
// scene frame (Y = -projected Y) back to the projected frame the batches use.
std::vector<std::vector<Pt>> ringsFromPath(const QPainterPath& path) {
    std::vector<std::vector<Pt>> rings;
    std::vector<Pt> ring;
    auto flush = [&]() {
        if (!ring.empty()) rings.push_back(std::move(ring));
        ring.clear();
    };
    const int n = path.elementCount();
    ring.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const QPainterPath::Element e = path.elementAt(i);
        if (e.type == QPainterPath::MoveToElement) {
            flush();
            ring.push_back({ e.x, -e.y });
        } else if (e.type == QPainterPath::LineToElement) {
            ring.push_back({ e.x, -e.y });
        }
    }
    flush();
    return rings;
}

// Convex hull (Andrew's monotone chain), CCW, collinear points dropped. O(k log
// k). Used only as the bounded fallback fill for a ring that failed its area
// check — small input (already-simplified vertices), so this is cheap.
std::vector<Pt> convexHull(std::vector<Pt> pts) {
    std::sort(pts.begin(), pts.end(), [](const Pt& a, const Pt& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    pts.erase(std::unique(pts.begin(), pts.end(), samePoint), pts.end());
    const int n = static_cast<int>(pts.size());
    if (n < 3) return pts;

    auto cross = [](const Pt& o, const Pt& a, const Pt& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    std::vector<Pt> h(static_cast<std::size_t>(2 * n));
    int k = 0;
    for (int i = 0; i < n; ++i) {                        // lower hull
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0.0) --k;
        h[k++] = pts[i];
    }
    for (int i = n - 2, t = k + 1; i >= 0; --i) {        // upper hull
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i]) <= 0.0) --k;
        h[k++] = pts[i];
    }
    h.resize(static_cast<std::size_t>(k - 1));
    return h;
}

// Fill the convex hull of `verts` as a triangle fan. Bounded fallback for a fill
// whose simplified rings didn't triangulate cleanly: it fills the ring's own
// extent (a concavity fills in — a small local loss at zoom-out scales), never a
// cell-spanning triangle, and costs nothing near the full-resolution path.
void appendConvexHullFan(const std::vector<Pt>& verts, const Rgb& col,
                         double originX, double originY,
                         std::vector<GpuVertex>& tris) {
    const std::vector<Pt> hull = convexHull(verts);
    if (hull.size() < 3) return;
    tris.reserve(tris.size() + (hull.size() - 2) * 3);
    for (std::size_t i = 1; i + 1 < hull.size(); ++i) {
        const Pt fan[3] = { hull[0], hull[i], hull[i + 1] };
        for (const Pt& p : fan)
            tris.push_back({ static_cast<float>(p.x - originX),
                             static_cast<float>(p.y - originY),
                             col.r, col.g, col.b });
    }
}

} // namespace

void appendBuiltCellFills(const BuiltCell& cell,
                          double originX, double originY,
                          std::vector<GpuVertex>& tris) {
    for (const BuiltPath& bp : cell.paths) {
        if (!bp.filled) continue;
        const Rgb col = toRgb(bp.brush);

        const std::size_t start = tris.size();
        if (appendTriangulatedPathRings(ringsFromPath(bp.path), col,
                                        originX, originY, tris))
            continue;   // simplified geometry triangulated cleanly (the common case)

        // Fallback: the simplified path failed its area check (self-intersecting
        // or collapsed). Discard its triangles and fill the convex hull of the
        // same simplified vertices — bounded and cheap, so a bad ring can never
        // stall the build the way re-triangulating full-resolution rings would.
        tris.resize(start);
        std::vector<Pt> verts;
        for (const std::vector<Pt>& r : ringsFromPath(bp.path))
            verts.insert(verts.end(), r.begin(), r.end());
        appendConvexHullFan(verts, col, originX, originY, tris);
    }
}

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

    // Legacy area fills: older prepared-render caches may still carry Stage 5
    // pre-triangulated batches. Current GPU builds append normal fill triangles
    // from clipped/simplified BuiltPath geometry before calling this function.
    // Keep this branch for compatibility and for tests covering malformed
    // PreparedFill input.
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
