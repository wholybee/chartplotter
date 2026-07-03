#pragma once
// src/geom_tessellate.hpp
//
// Stage 5: ear-clipping triangulator for polygon rings.
//
// Pre-triangulates area fills at prepare time so the retained/GPU backend
// (Stage 7) can draw them as indexed triangles instead of re-tessellating in
// the frame loop. Holes are handled by earcut-style hole elimination
// (mergeHoles): each hole is bridged into the outer ring through a two-way
// edge, producing one simple polygon that plain ear clipping triangulates.
// The QPainter renderer does not consume this — it fills QPainterPath from
// the rings directly with even-odd — so an imperfect result here can only
// affect the GPU backend's fills, never the painter's on-screen output.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
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

            // No other vertex inside the candidate ear triangle? Vertices that
            // exactly coincide with an ear corner don't block: hole-bridge
            // duplicates (mergeHoles) sit on top of corners by construction.
            bool ear = true;
            for (std::size_t k = 0; k < m; ++k) {
                const uint32_t ik = idx[k];
                if (ik == i0 || ik == i1 || ik == i2) continue;
                const Pt& q = ring[ik];
                if ((q.x == a.x && q.y == a.y) || (q.x == b.x && q.y == b.y) ||
                    (q.x == c.x && q.y == c.y)) continue;
                if (detail::pointInTri(q, a, b, c)) { ear = false; break; }
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

// Point-in-polygon (crossing test). Boundary points may land either way; the
// callers only classify well-separated rings, where that ambiguity is moot.
inline bool pointInRing(const Pt& p, const std::vector<Pt>& ring) {
    bool in = false;
    const std::size_t n = ring.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Pt& a = ring[j];
        const Pt& b = ring[i];
        if ((b.y > p.y) != (a.y > p.y) &&
            p.x < (a.x - b.x) * (p.y - b.y) / (a.y - b.y) + b.x)
            in = !in;
    }
    return in;
}

// Earcut-style hole elimination: bridge every hole into the outer ring through
// a two-way edge, producing one simple polygon (with duplicated bridge
// vertices) that triangulate() can ear-clip directly. The outer ring is
// oriented CCW and holes CW; each hole attaches at its rightmost vertex to a
// visible outer vertex found by casting a +x ray. A hole that never sees the
// outer ring (bad data) is skipped — it fills over, the pre-hole behaviour.
inline std::vector<Pt> mergeHoles(const std::vector<Pt>& outerIn,
                                  std::vector<const std::vector<Pt>*> holes) {
    std::vector<Pt> poly = outerIn;
    if (signedArea2(poly) < 0.0) std::reverse(poly.begin(), poly.end());

    // Rightmost holes first, so later bridges can pass through earlier ones'
    // bridge corridors without crossing them.
    auto maxX = [](const std::vector<Pt>& r) {
        double m = r[0].x;
        for (const Pt& p : r) m = std::max(m, p.x);
        return m;
    };
    std::sort(holes.begin(), holes.end(),
              [&](const std::vector<Pt>* a, const std::vector<Pt>* b) {
                  return maxX(*a) > maxX(*b);
              });

    for (const std::vector<Pt>* hp : holes) {
        std::vector<Pt> hole = *hp;
        if (signedArea2(hole) > 0.0) std::reverse(hole.begin(), hole.end());

        // M: the hole's rightmost vertex — the bridge leaves from here.
        std::size_t hm = 0;
        for (std::size_t i = 1; i < hole.size(); ++i)
            if (hole[i].x > hole[hm].x) hm = i;
        const Pt M = hole[hm];

        // Closest intersection I of the ray (M, +x) with an outer edge.
        const std::size_t n = poly.size();
        double bestX = std::numeric_limits<double>::max();
        std::size_t hitEdge = static_cast<std::size_t>(-1);
        for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
            const Pt& a = poly[j];
            const Pt& b = poly[i];
            if ((a.y > M.y) == (b.y > M.y)) continue;   // edge doesn't straddle
            const double x = a.x + (M.y - a.y) / (b.y - a.y) * (b.x - a.x);
            if (x >= M.x && x < bestX) { bestX = x; hitEdge = j; }
        }
        if (hitEdge == static_cast<std::size_t>(-1)) continue;   // hole outside
        const Pt I{ bestX, M.y };

        // Bridge vertex P: the intersected edge's endpoint with the larger x —
        // unless another vertex sits inside triangle (M, I, P), in which case
        // visibility demands the inside vertex closest in angle to the ray.
        const std::size_t e2 = (hitEdge + 1) % n;
        std::size_t pi = (poly[hitEdge].x > poly[e2].x) ? hitEdge : e2;
        double bestTan = std::numeric_limits<double>::max();
        for (std::size_t k = 0; k < n; ++k) {
            if (k == pi) continue;
            const Pt& q = poly[k];
            if (q.x < M.x) continue;
            if (q.x == M.x && q.y == M.y) continue;
            if (!detail::pointInTri(q, M, I, poly[pi])) continue;
            const double dx = q.x - M.x;
            const double tan = (dx > 0.0)
                ? std::abs(q.y - M.y) / dx
                : std::numeric_limits<double>::max();
            if (tan < bestTan) { bestTan = tan; pi = k; }
        }

        // Splice the hole in through the bridge: ... P, M, (hole cycle), M, P ...
        std::vector<Pt> ins;
        ins.reserve(hole.size() + 2);
        for (std::size_t k = 0; k <= hole.size(); ++k)
            ins.push_back(hole[(hm + k) % hole.size()]);
        ins.push_back(poly[pi]);
        poly.insert(poly.begin() + static_cast<std::ptrdiff_t>(pi) + 1,
                    ins.begin(), ins.end());
    }
    return poly;
}

} // namespace geomtess
