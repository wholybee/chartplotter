#pragma once
#include <cmath>
#include <vector>
#include "chart_loader.hpp"   // Pt, BBox

// Pure geometry: clipping polygons and polylines to an axis-aligned rectangle,
// in projected (Mercator metre) coordinates. No Qt, no GDAL — just math, so it
// can be unit-tested in isolation and reused by the renderer.
//
// The renderer caches each cell's full parse and clips at scene-build time to a
// region a little larger than the viewport. A coarse cell that only shows a
// sliver via gap-fill then contributes a screen-sized polygon instead of a
// basin-spanning one, so Qt traverses and rasterizes far less per frame. The
// clip region is always larger than the visible viewport, so the straight edges
// clipping introduces fall off-screen.
namespace geom {

// Sutherland–Hodgman: clip one polygon ring to a rect. The rect is convex, so a
// single ring clips to a single ring (possibly empty). An explicit closing
// duplicate vertex is tolerated and dropped.
inline std::vector<Pt> clipRingToRect(const std::vector<Pt>& in, const BBox& r) {
    if (in.size() < 3) return {};
    std::vector<Pt> poly(in.begin(), in.end());
    if (poly.size() >= 2 &&
        poly.front().x == poly.back().x && poly.front().y == poly.back().y)
        poly.pop_back();
    if (poly.size() < 3) return {};

    enum Side { L, R, B, T };
    auto inside = [&](const Pt& p, Side s) {
        switch (s) {
            case L: return p.x >= r.minx;
            case R: return p.x <= r.maxx;
            case B: return p.y >= r.miny;
            case T: return p.y <= r.maxy;
        }
        return true;
    };
    // Only called when endpoints straddle the boundary, so the denominator is
    // nonzero.
    auto isect = [&](const Pt& a, const Pt& b, Side s) {
        Pt o{}; double t;
        switch (s) {
            case L: t = (r.minx - a.x) / (b.x - a.x); o.x = r.minx; o.y = a.y + t * (b.y - a.y); break;
            case R: t = (r.maxx - a.x) / (b.x - a.x); o.x = r.maxx; o.y = a.y + t * (b.y - a.y); break;
            case B: t = (r.miny - a.y) / (b.y - a.y); o.y = r.miny; o.x = a.x + t * (b.x - a.x); break;
            case T: t = (r.maxy - a.y) / (b.y - a.y); o.y = r.maxy; o.x = a.x + t * (b.x - a.x); break;
        }
        return o;
    };
    for (Side s : {L, R, B, T}) {
        if (poly.empty()) break;
        std::vector<Pt> out;
        out.reserve(poly.size() + 4);
        const std::size_t n = poly.size();
        for (std::size_t i = 0; i < n; ++i) {
            const Pt& cur  = poly[i];
            const Pt& prev = poly[(i + n - 1) % n];
            const bool curIn  = inside(cur, s);
            const bool prevIn = inside(prev, s);
            if (curIn) {
                if (!prevIn) out.push_back(isect(prev, cur, s));
                out.push_back(cur);
            } else if (prevIn) {
                out.push_back(isect(prev, cur, s));
            }
        }
        poly.swap(out);
    }
    return poly;
}

inline int outcode(const Pt& p, const BBox& r) {
    int c = 0;
    if (p.x < r.minx) c |= 1; else if (p.x > r.maxx) c |= 2;
    if (p.y < r.miny) c |= 4; else if (p.y > r.maxy) c |= 8;
    return c;
}

// Cohen–Sutherland clip of one segment to a rect.
inline bool clipSegment(Pt a, Pt b, const BBox& r, Pt& oa, Pt& ob) {
    int ca = outcode(a, r), cb = outcode(b, r);
    for (;;) {
        if (!(ca | cb)) { oa = a; ob = b; return true; }   // both inside
        if (ca & cb)    return false;                       // share an outside half-plane
        const int c = ca ? ca : cb;
        Pt p{};
        if      (c & 8) { p.x = a.x + (b.x - a.x) * (r.maxy - a.y) / (b.y - a.y); p.y = r.maxy; }
        else if (c & 4) { p.x = a.x + (b.x - a.x) * (r.miny - a.y) / (b.y - a.y); p.y = r.miny; }
        else if (c & 2) { p.y = a.y + (b.y - a.y) * (r.maxx - a.x) / (b.x - a.x); p.x = r.maxx; }
        else            { p.y = a.y + (b.y - a.y) * (r.minx - a.x) / (b.x - a.x); p.x = r.minx; }
        if (c == ca) { a = p; ca = outcode(a, r); }
        else         { b = p; cb = outcode(b, r); }
    }
}

// Clip a polyline to a rect, stitching adjacent visible segments into
// contiguous runs so the renderer emits one subpath per run, not per segment.
inline std::vector<std::vector<Pt>> clipPolylineToRect(const std::vector<Pt>& line,
                                                       const BBox& r) {
    std::vector<std::vector<Pt>> runs;
    if (line.size() < 2) return runs;
    constexpr double eps = 1e-6;
    for (std::size_t i = 1; i < line.size(); ++i) {
        Pt a, b;
        if (!clipSegment(line[i - 1], line[i], r, a, b)) continue;
        if (!runs.empty()) {
            const Pt& last = runs.back().back();
            if (std::fabs(last.x - a.x) <= eps && std::fabs(last.y - a.y) <= eps) {
                runs.back().push_back(b);
                continue;
            }
        }
        runs.push_back({a, b});
    }
    return runs;
}

inline bool pointInRect(const Pt& p, const BBox& r) {
    return p.x >= r.minx && p.x <= r.maxx && p.y >= r.miny && p.y <= r.maxy;
}

// Douglas–Peucker line simplification: drop vertices that lie within `tol`
// (projected metres) of the polyline they'd otherwise add detail to. Endpoints
// are always kept, so closed rings stay closed. Iterative (explicit stack) to
// avoid deep recursion on long coastlines. With tol <= 0 the input is returned
// unchanged. Used at scene-build time to shed vertices that would be smaller
// than a fraction of a pixel at the band's display scale.
inline std::vector<Pt> simplify(const std::vector<Pt>& pts, double tol) {
    const int n = static_cast<int>(pts.size());
    if (n < 3 || tol <= 0.0) return pts;

    const double tol2 = tol * tol;
    std::vector<bool> keep(n, false);
    keep[0] = keep[n - 1] = true;

    std::vector<std::pair<int, int>> stack;
    stack.emplace_back(0, n - 1);
    while (!stack.empty()) {
        const int a = stack.back().first, b = stack.back().second;
        stack.pop_back();

        const double ax = pts[a].x, ay = pts[a].y;
        const double dx = pts[b].x - ax, dy = pts[b].y - ay;
        const double len2 = dx * dx + dy * dy;

        double maxD2 = 0.0;
        int idx = -1;
        for (int i = a + 1; i < b; ++i) {
            double d2;
            if (len2 <= 0.0) {                       // a == b: distance to the point
                const double px = pts[i].x - ax, py = pts[i].y - ay;
                d2 = px * px + py * py;
            } else {
                double t = ((pts[i].x - ax) * dx + (pts[i].y - ay) * dy) / len2;
                t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
                const double ex = pts[i].x - (ax + t * dx);
                const double ey = pts[i].y - (ay + t * dy);
                d2 = ex * ex + ey * ey;
            }
            if (d2 > maxD2) { maxD2 = d2; idx = i; }
        }

        if (idx > 0 && maxD2 > tol2) {
            keep[idx] = true;
            stack.emplace_back(a, idx);
            stack.emplace_back(idx, b);
        }
    }

    std::vector<Pt> out;
    out.reserve(n);
    for (int i = 0; i < n; ++i)
        if (keep[i]) out.push_back(pts[i]);
    return out;
}

namespace detail {

// Douglas–Peucker over one arc of a ring, given as an ordered index list `ids`
// into `pts`. The arc's endpoints (ids.front()/ids.back()) are anchors kept by
// the caller; this marks keep[] for interior vertices whose deviation from the
// arc chord exceeds sqrt(tol2). Iterative (explicit stack) like simplify().
inline void dpArc(const std::vector<Pt>& pts, const std::vector<int>& ids,
                  double tol2, std::vector<bool>& keep) {
    const int m = static_cast<int>(ids.size());
    if (m < 3) return;
    std::vector<std::pair<int, int>> stack;   // [lo,hi] indices into ids
    stack.emplace_back(0, m - 1);
    while (!stack.empty()) {
        const int lo = stack.back().first, hi = stack.back().second;
        stack.pop_back();
        if (hi - lo < 2) continue;

        const double ax = pts[ids[lo]].x, ay = pts[ids[lo]].y;
        const double dx = pts[ids[hi]].x - ax, dy = pts[ids[hi]].y - ay;
        const double len2 = dx * dx + dy * dy;

        double maxD2 = 0.0;
        int split = -1;
        for (int k = lo + 1; k < hi; ++k) {
            const double px = pts[ids[k]].x, py = pts[ids[k]].y;
            double d2;
            if (len2 <= 0.0) {                    // degenerate chord: distance to endpoint
                const double ex = px - ax, ey = py - ay;
                d2 = ex * ex + ey * ey;
            } else {
                double t = ((px - ax) * dx + (py - ay) * dy) / len2;
                t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
                const double ex = px - (ax + t * dx), ey = py - (ay + t * dy);
                d2 = ex * ex + ey * ey;
            }
            if (d2 > maxD2) { maxD2 = d2; split = k; }
        }

        if (split > 0 && maxD2 > tol2) {
            keep[ids[split]] = true;
            stack.emplace_back(lo, split);
            stack.emplace_back(split, hi);
        }
    }
}

} // namespace detail

// Ring-aware Douglas–Peucker: simplify a CLOSED ring without the endpoint
// artifacts of the open simplify(). simplify() force-keeps pts[0] and pts[n-1];
// for a ring stored open (first != last) those are *adjacent* vertices, so the
// whole ring gets approximated against one short baseline edge — which at large
// zoom-out tolerances collapses area rings into wildly wrong shapes (or makes
// them self-intersect). Instead, anchor two vertices that are far apart (a
// coordinate extreme and the vertex farthest from it) and simplify the two arcs
// between them, treating the ring cyclically. This removes the gross deformation
// that produced spurious "large triangles" in GPU fills; the tessellator's area
// check (gpu_batches) remains the backstop for any residual non-simple result.
inline std::vector<Pt> simplifyRing(const std::vector<Pt>& pts, double tol) {
    const int n = static_cast<int>(pts.size());
    if (n < 4 || tol <= 0.0) return pts;   // a triangle can't be simplified further

    // Anchor A: lexicographic coordinate extreme (deterministic, on the hull).
    int a = 0;
    for (int i = 1; i < n; ++i)
        if (pts[i].x < pts[a].x || (pts[i].x == pts[a].x && pts[i].y < pts[a].y))
            a = i;
    // Anchor B: the vertex farthest from A, so the two anchors span the ring.
    int b = a;
    double best = -1.0;
    for (int i = 0; i < n; ++i) {
        const double dx = pts[i].x - pts[a].x, dy = pts[i].y - pts[a].y;
        const double d2 = dx * dx + dy * dy;
        if (d2 > best) { best = d2; b = i; }
    }
    if (b == a) return pts;   // all vertices coincide — nothing to do

    std::vector<bool> keep(n, false);
    keep[a] = keep[b] = true;

    std::vector<int> arc;
    arc.reserve(static_cast<std::size_t>(n) + 1);
    auto buildArc = [&](int from, int to) {          // indices from..to, cyclic
        arc.clear();
        int i = from;
        arc.push_back(i);
        while (i != to) { i = (i + 1) % n; arc.push_back(i); }
    };

    const double tol2 = tol * tol;
    buildArc(a, b); detail::dpArc(pts, arc, tol2, keep);
    buildArc(b, a); detail::dpArc(pts, arc, tol2, keep);

    std::vector<Pt> out;
    out.reserve(n);
    for (int i = 0; i < n; ++i)
        if (keep[i]) out.push_back(pts[i]);
    return out;
}

} // namespace geom
