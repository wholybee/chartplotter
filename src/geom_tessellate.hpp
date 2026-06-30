#pragma once
// src/geom_tessellate.hpp
//
// Stage 5: ear-clipping triangulator for a simple polygon ring.
//
// Pre-triangulates area fills at prepare time so the retained/GPU backend
// (Stage 7) can draw them as indexed triangles instead of re-tessellating in the
// frame loop. Exterior-ring only: holes are ignored for now (the architecture
// plan permits simple-ring ear clipping initially and a robust hole-aware
// tessellator later). The current QPainter renderer does not consume this — it
// fills QPainterPath from the rings directly — so an imperfect result here cannot
// affect today's on-screen output.

#include <algorithm>
#include <cstdint>
#include <vector>
#include "chart_loader.hpp"   // Pt

namespace geomtess {

// Signed area*2 of a ring (CCW positive).
inline double signedArea2(const std::vector<Pt>& r) {
    double a = 0.0;
    const std::size_t n = r.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++)
        a += r[j].x * r[i].y - r[i].x * r[j].y;
    return a;
}

namespace detail {

inline bool pointInTri(const Pt& p, const Pt& a, const Pt& b, const Pt& c) {
    const double d1 = (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y);
    const double d2 = (p.x - c.x) * (b.y - c.y) - (b.x - c.x) * (p.y - c.y);
    const double d3 = (p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y);
    const bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    const bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(hasNeg && hasPos);   // inside (or on edge) when all same sign
}

} // namespace detail

// Triangulate a simple polygon ring into triangle index triples into `ring`.
// Returns an empty vector for degenerate input (< 3 vertices). The result has
// 3*(n-2) indices for a clean simple polygon; a self-intersecting or otherwise
// pathological ring may yield fewer (best effort).
inline std::vector<uint32_t> triangulate(const std::vector<Pt>& ring) {
    std::vector<uint32_t> out;
    const std::size_t n = ring.size();
    if (n < 3) return out;

    // Work on an index list; orient CCW so the convex/ear tests are consistent.
    std::vector<uint32_t> idx(n);
    for (std::size_t i = 0; i < n; ++i) idx[i] = static_cast<uint32_t>(i);
    if (signedArea2(ring) < 0.0)
        std::reverse(idx.begin(), idx.end());

    out.reserve(3 * (n - 2));
    int guard = 0;
    const int guardMax = static_cast<int>(n) * static_cast<int>(n) + 16;

    while (idx.size() > 2 && guard++ < guardMax) {
        const std::size_t m = idx.size();
        bool clipped = false;
        for (std::size_t i = 0; i < m; ++i) {
            const uint32_t i0 = idx[(i + m - 1) % m];
            const uint32_t i1 = idx[i];
            const uint32_t i2 = idx[(i + 1) % m];
            const Pt& a = ring[i0];
            const Pt& b = ring[i1];
            const Pt& c = ring[i2];

            // Convex vertex? (cross > 0 for CCW)
            const double cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (cross <= 0.0) continue;

            // No other vertex inside the candidate ear triangle?
            bool ear = true;
            for (std::size_t k = 0; k < m; ++k) {
                const uint32_t ik = idx[k];
                if (ik == i0 || ik == i1 || ik == i2) continue;
                if (detail::pointInTri(ring[ik], a, b, c)) { ear = false; break; }
            }
            if (!ear) continue;

            out.push_back(i0);
            out.push_back(i1);
            out.push_back(i2);
            idx.erase(idx.begin() + i);
            clipped = true;
            break;
        }
        if (!clipped) break;   // no ear found (degenerate) — stop best-effort
    }
    return out;
}

} // namespace geomtess
