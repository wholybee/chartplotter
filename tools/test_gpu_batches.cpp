// tools/test_gpu_batches.cpp
//
// Unit test for the built-cell -> GPU batch converter (src/gpu_batches.cpp,
// Stage 7). Verifies current fills come from clipped/simplified BuiltPath
// geometry, legacy PreparedFill batches still work, lines use the painter's pen
// rules, depth contours land in their own bucket, and vertices are emitted
// origin-relative. No Qt event loop, no GDAL, no RHI.

#include "gpu_batches.hpp"
#include "cell_builder.hpp"
#include "chart_loader.hpp"
#include "geom_clip.hpp"        // geom::simplifyRing (D)
#include "prepared_render.hpp"

#include <QColor>
#include <QPainterPath>

#include <cmath>
#include <cstdio>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) { std::printf("FAIL: %s\n", (msg)); ++g_failures; }        \
        else         { std::printf("ok:   %s\n", (msg)); }                      \
    } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 0.5f / 255.f; }
static bool rgbNear(const GpuVertex& v, int r, int g, int b) {
    return near(v.r, r / 255.f) && near(v.g, g / 255.f) && near(v.b, b / 255.f);
}
static int countRgb(const std::vector<GpuVertex>& verts, int r, int g, int b) {
    int count = 0;
    for (const GpuVertex& v : verts)
        if (rgbNear(v, r, g, b)) ++count;
    return count;
}
static double triArea(const std::vector<GpuVertex>& verts) {
    double area = 0.0;
    for (std::size_t i = 0; i + 2 < verts.size(); i += 3) {
        const GpuVertex& a = verts[i];
        const GpuVertex& b = verts[i + 1];
        const GpuVertex& c = verts[i + 2];
        area += std::fabs((b.x - a.x) * (c.y - a.y) -
                          (c.x - a.x) * (b.y - a.y)) * 0.5;
    }
    return area;
}

// A unit square ring, triangulated as two triangles {0,1,2, 0,2,3}.
static PreparedFill squareFill(quint32 fidx, float x0, float y0, float side) {
    PreparedFill pf;
    pf.featureIndex = fidx;
    pf.verts = { x0, y0,  x0 + side, y0,  x0 + side, y0 + side,  x0, y0 + side };
    pf.indices = { 0, 1, 2, 0, 2, 3 };
    return pf;
}

static void setBBoxes(std::vector<Feature>& feats) {
    for (Feature& f : feats)
        for (const auto& ring : f.rings)
            for (const Pt& p : ring) f.bbox.expand(p.x, p.y);
}

// Shoelace area (signed) of a ring.
static double polyArea(const std::vector<Pt>& r) {
    double a = 0.0;
    const std::size_t n = r.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++)
        a += r[j].x * r[i].y - r[i].x * r[j].y;
    return a * 0.5;
}

// True if segments ab and cd cross at an interior point (strict).
static bool segCross(const Pt& a, const Pt& b, const Pt& c, const Pt& d) {
    auto cr = [](const Pt& o, const Pt& p, const Pt& q) {
        return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x);
    };
    const double d1 = cr(c, d, a), d2 = cr(c, d, b);
    const double d3 = cr(a, b, c), d4 = cr(a, b, d);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

// O(n^2) simplicity check: no two non-adjacent edges of the closed ring cross.
static bool isSimplePolygon(const std::vector<Pt>& r) {
    const int n = static_cast<int>(r.size());
    if (n < 4) return true;
    for (int i = 0; i < n; ++i) {
        const Pt& a = r[i];
        const Pt& b = r[(i + 1) % n];
        for (int j = i + 1; j < n; ++j) {
            if (j == (i + 1) % n || (j + 1) % n == i) continue;   // adjacent edges
            if (segCross(a, b, r[j], r[(j + 1) % n])) return false;
        }
    }
    return true;
}

int main() {
    std::vector<Feature> feats(5);
    // 0: depth area, DRVAL ~8 m -> fill (184,217,240), no outline.
    feats[0].kind = FeatureKind::DepthArea; feats[0].hasDepth = true; feats[0].depth = 8.0;
    feats[0].rings = { { {0,0},{100,0},{100,100},{0,100} } };
    // 1: land area -> fill (217,199,148), outline (115,97,64).
    feats[1].kind = FeatureKind::LandArea;
    feats[1].rings = { { {200,0},{300,0},{300,100},{200,100} } };
    // 2: coastline -> open outline (64,51,31), no fill.
    feats[2].kind = FeatureKind::Coastline;
    feats[2].rings = { { {0,0},{50,50},{100,0} } };
    // 3: OtherArea with an AC() wash from portrayal -> fill from hit, no outline.
    feats[3].kind = FeatureKind::OtherArea;
    feats[3].rings = { { {0,200},{100,200},{100,300},{0,300} } };
    // 4: depth contour -> separate contour bucket, pen (115,153,199).
    feats[4].kind = FeatureKind::DepthContour;
    feats[4].rings = { { {10,10},{50,40},{90,10} } };
    setBBoxes(feats);

    PreparedCellRender prep;
    prep.hits.resize(feats.size());
    prep.hasHit.assign(feats.size(), 0);
    prep.hits[3].hasFill = true;
    prep.hits[3].fill = SymFillStyle{ 10, 20, 30, 255 };
    prep.hasHit[3] = 1;

    prep.fills.push_back(squareFill(0, 0, 0, 100));       // depth
    prep.fills.push_back(squareFill(1, 200, 0, 100));     // land
    prep.fills.push_back(squareFill(3, 0, 200, 100));     // OtherArea wash

    // Unclipped, unsimplified build: an invalid clip box disables clipping and
    // tol 0 disables simplification, so counts are exact.
    const BuiltCell cell =
        cellbuilder::instantiateCell(QString(), feats, prep, 0, BBox{}, 0.0);

    std::vector<GpuVertex> tris, lines, contours;
    gpubatches::appendCellBatches(feats, prep, cell, /*originX*/0.0, /*originY*/0.0,
                                  tris, lines, contours);

    // Three fills x 6 indices each = 18 triangle vertices.
    CHECK(tris.size() == 18, "fills: 18 triangle vertices (3 quads)");
    CHECK(rgbNear(tris[0], 184, 217, 240), "depth-area fill colour matches painter");
    CHECK(rgbNear(tris[6], 217, 199, 148), "land-area fill colour matches painter");
    CHECK(rgbNear(tris[12], 10, 20, 30),   "OtherArea fill takes portrayal AC() colour");

    // Outlines from BuiltPath: depth area none (no pen); land closed quad
    // (4 segs = 8 verts); coast open (2 segs = 4); OtherArea fill-only wash ->
    // none. Total 12 line vertices.
    CHECK(lines.size() == 12, "outlines: 12 line vertices (land quad + coast)");
    bool anyLand = false, anyCoast = false;
    for (const GpuVertex& v : lines) {
        if (rgbNear(v, 115, 97, 64)) anyLand = true;
        if (rgbNear(v, 64, 51, 31))  anyCoast = true;
    }
    CHECK(anyLand,  "land outline uses the painter's land pen");
    CHECK(anyCoast, "coastline outline uses the painter's coast pen");

    // Depth contour: its own bucket (2 segs = 4 verts), painter's contour pen,
    // and none of it leaked into the regular lines bucket.
    CHECK(contours.size() == 4, "depth contour: 4 vertices in the contour bucket");
    CHECK(!contours.empty() && rgbNear(contours[0], 115, 153, 199),
          "depth contour uses the painter's contour pen");
    bool contourInLines = false;
    for (const GpuVertex& v : lines)
        if (rgbNear(v, 115, 153, 199)) contourInLines = true;
    CHECK(!contourInLines, "no contour vertices in the regular lines bucket");

    // Lines follow the painter's scene-frame geometry mapped back to the
    // projected frame: the coastline apex (50,50) appears with projected Y
    // (not the scene frame's negated Y).
    bool apexOk = false;
    for (const GpuVertex& v : lines)
        if (rgbNear(v, 64, 51, 31) && near(v.x, 50.f) && near(v.y, 50.f))
            apexOk = true;
    CHECK(apexOk, "line vertices are in the projected frame (Y north-up)");

    // Origin-relative emission: shift the origin and confirm the offset applies.
    std::vector<GpuVertex> tris2, lines2, contours2;
    gpubatches::appendCellBatches(feats, prep, cell, /*originX*/10.0, /*originY*/20.0,
                                  tris2, lines2, contours2);
    CHECK(near(tris2[0].x, tris[0].x - 10.0f) && near(tris2[0].y, tris[0].y - 20.0f),
          "fill vertices are emitted relative to the origin");
    CHECK(near(lines2[0].x, lines[0].x - 10.0f) && near(lines2[0].y, lines[0].y - 20.0f),
          "line vertices are emitted relative to the origin");

    // Basemap path: fills can be triangulated from the already-instantiated
    // BuiltCell, avoiding pre-triangulation of raw unclipped source polygons.
    std::vector<GpuVertex> builtFillTris;
    gpubatches::appendBuiltCellFills(cell, 0.0, 0.0, builtFillTris);
    CHECK(builtFillTris.size() == 18, "built-cell fills: 18 triangle vertices (3 quads)");
    CHECK(countRgb(builtFillTris, 184, 217, 240) == 6,
          "built-cell fills: depth-area colour matches painter");
    CHECK(countRgb(builtFillTris, 217, 199, 148) == 6,
          "built-cell fills: land-area colour matches painter");
    CHECK(countRgb(builtFillTris, 10, 20, 30) == 6,
          "built-cell fills: OtherArea wash colour matches painter");

    std::vector<Feature> holeFeats(1);
    holeFeats[0].kind = FeatureKind::LandArea;
    holeFeats[0].rings = {
        { {0, 0}, {100, 0}, {100, 100}, {0, 100} },
        { {20, 20}, {80, 20}, {80, 80}, {20, 80} }
    };
    setBBoxes(holeFeats);
    PreparedCellRender holePrep;
    holePrep.hits.resize(holeFeats.size());
    holePrep.hasHit.assign(holeFeats.size(), 0);
    const BuiltCell holeCell =
        cellbuilder::instantiateCell(QString(), holeFeats, holePrep, 0, BBox{}, 0.0);
    std::vector<GpuVertex> holeTris;
    gpubatches::appendBuiltCellFills(holeCell, 0.0, 0.0, holeTris);
    CHECK(!holeTris.empty(), "built-cell fills: holed polygon triangulates");
    CHECK(std::fabs(triArea(holeTris) - 6400.0) < 1.0,
          "built-cell fills: holed polygon preserves odd-even fill area");

    // Clip filtering: a clip box around the left column only. The land quad
    // (x 200..300) and the OtherArea wash (y 200..300) are outside — their fill
    // triangles must be dropped, leaving the depth-area quad's 6 vertices. The
    // land outline disappears too (its BuiltPath was clipped away).
    BBox clip;
    clip.expand(-10.0, -10.0);
    clip.expand(120.0, 110.0);
    const BuiltCell clipped =
        cellbuilder::instantiateCell(QString(), feats, prep, 0, clip, 0.0);
    std::vector<GpuVertex> trisC, linesC, contoursC;
    gpubatches::appendCellBatches(feats, prep, clipped, 0.0, 0.0,
                                  trisC, linesC, contoursC);
    CHECK(trisC.size() == 6, "clip: out-of-box fill triangles are dropped");
    CHECK(rgbNear(trisC[0], 184, 217, 240), "clip: surviving fill is the depth area");
    std::vector<GpuVertex> builtFillTrisC;
    gpubatches::appendBuiltCellFills(clipped, 0.0, 0.0, builtFillTrisC);
    CHECK(builtFillTrisC.size() == 6,
          "built-cell fills clip: out-of-box fill paths are dropped before triangulation");
    CHECK(rgbNear(builtFillTrisC[0], 184, 217, 240),
          "built-cell fills clip: surviving fill is the depth area");
    bool landInClipped = false;
    for (const GpuVertex& v : linesC)
        if (rgbNear(v, 115, 97, 64)) landInClipped = true;
    CHECK(!landInClipped, "clip: out-of-box outlines are dropped");
    CHECK(contoursC.size() == 4, "clip: in-box contour survives");

    // ---- D: ring-aware simplification ----------------------------------------
    // A 64-gon (radius 500) with fine sub-tolerance boundary serration. The
    // ring-aware simplifier must shed vertices, keep the ring simple, and
    // preserve area — the endpoint-anchored open simplifier would approximate
    // the whole ring against one short baseline edge and could deform it badly.
    {
        std::vector<Pt> ring;
        const int N = 64;
        for (int i = 0; i < N; ++i) {
            const double ang = 2.0 * 3.14159265358979323846 * i / N;
            const double rad = 500.0 + ((i % 2) ? 6.0 : -6.0);   // ±6 m (< tol)
            ring.push_back({ rad * std::cos(ang), rad * std::sin(ang) });
        }
        const std::vector<Pt> simp = geom::simplifyRing(ring, 40.0);
        CHECK(simp.size() >= 3 && simp.size() < ring.size(),
              "D: simplifyRing sheds sub-tolerance vertices");
        CHECK(isSimplePolygon(simp),
              "D: simplified ring stays simple (no self-intersection)");
        const double a0 = std::fabs(polyArea(ring));
        const double a1 = std::fabs(polyArea(simp));
        CHECK(a0 > 0.0 && std::fabs(a1 - a0) < 0.08 * a0,
              "D: simplified ring preserves area within 8%");
        // A bare triangle can't be simplified further (guarded, returned as-is).
        const std::vector<Pt> tri = { {0, 0}, {100, 0}, {50, 100} };
        CHECK(geom::simplifyRing(tri, 1000.0).size() == 3,
              "D: simplifyRing leaves a triangle intact");
    }

    // ---- E: area-validated fill drops a degenerate ring (no giant fallback) ---
    // A fill path shaped as a self-intersecting bowtie — the degenerate ring
    // aggressive simplification can rarely emit. The area check must reject the
    // bowtie's triangulation and append NOTHING for it. A convex-hull substitute
    // would flood the ring's whole extent with the fill colour (the giant-
    // triangle artifact for a concave/basemap ring), so the fill is dropped: its
    // outline still strokes elsewhere and the layer beneath shows through.
    {
        BuiltCell bad;
        BuiltPath bp;
        bp.filled = true;
        bp.brush = QColor(217, 199, 148);
        bp.path.moveTo(0, 0);                // edges (0,0)-(100,100) and
        bp.path.lineTo(100, 100);            // (100,0)-(0,100) cross => bowtie
        bp.path.lineTo(100, 0);
        bp.path.lineTo(0, 100);
        bp.path.closeSubpath();
        bad.paths.push_back(bp);

        std::vector<GpuVertex> ft;
        gpubatches::appendBuiltCellFills(bad, 0.0, 0.0, ft);
        CHECK(ft.empty(),
              "E: rejected bowtie appends no fill (no cell-spanning fallback)");
    }

    if (g_failures == 0) { std::printf("\nAll gpu_batches tests passed.\n"); return 0; }
    std::printf("\n%d gpu_batches test(s) failed.\n", g_failures);
    return 1;
}
