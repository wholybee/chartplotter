// tools/test_gpu_batches.cpp
//
// Unit test for the prepared-render -> GPU batch converter (src/gpu_batches.cpp,
// Stage 7 A4). Verifies fills come from the pre-triangulated PreparedFill with
// painter-matching colours, that outlines follow the painter's pen rules, and
// that vertices are emitted origin-relative. No Qt event loop, no GDAL, no RHI.

#include "gpu_batches.hpp"
#include "chart_loader.hpp"
#include "prepared_render.hpp"

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

// A unit square ring, triangulated as two triangles {0,1,2, 0,2,3}.
static PreparedFill squareFill(quint32 fidx, float x0, float y0, float side) {
    PreparedFill pf;
    pf.featureIndex = fidx;
    pf.verts = { x0, y0,  x0 + side, y0,  x0 + side, y0 + side,  x0, y0 + side };
    pf.indices = { 0, 1, 2, 0, 2, 3 };
    return pf;
}

int main() {
    std::vector<Feature> feats(4);
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

    PreparedCellRender prep;
    prep.hits.resize(feats.size());
    prep.hasHit.assign(feats.size(), 0);
    prep.hits[3].hasFill = true;
    prep.hits[3].fill = SymFillStyle{ 10, 20, 30, 255 };
    prep.hasHit[3] = 1;

    prep.fills.push_back(squareFill(0, 0, 0, 100));       // depth
    prep.fills.push_back(squareFill(1, 200, 0, 100));     // land
    prep.fills.push_back(squareFill(3, 0, 200, 100));     // OtherArea wash

    std::vector<GpuVertex> tris, lines;
    gpubatches::appendCellBatches(feats, prep, /*originX*/0.0, /*originY*/0.0, tris, lines);

    // Three fills x 6 indices each = 18 triangle vertices.
    CHECK(tris.size() == 18, "fills: 18 triangle vertices (3 quads)");
    CHECK(rgbNear(tris[0], 184, 217, 240), "depth-area fill colour matches painter");
    CHECK(rgbNear(tris[6], 217, 199, 148), "land-area fill colour matches painter");
    CHECK(rgbNear(tris[12], 10, 20, 30),   "OtherArea fill takes portrayal AC() colour");

    // Outlines: depth none; land closed (4 segs=8 verts); coast open (2 segs=4);
    // OtherArea fill-only wash -> none. Total 12 line vertices.
    CHECK(lines.size() == 12, "outlines: 12 line vertices (land quad + coast)");
    bool anyLand = false, anyCoast = false;
    for (const GpuVertex& v : lines) {
        if (rgbNear(v, 115, 97, 64)) anyLand = true;
        if (rgbNear(v, 64, 51, 31))  anyCoast = true;
    }
    CHECK(anyLand,  "land outline uses the painter's land pen");
    CHECK(anyCoast, "coastline outline uses the painter's coast pen");

    // Origin-relative emission: shift the origin and confirm the offset applies.
    std::vector<GpuVertex> tris2, lines2;
    gpubatches::appendCellBatches(feats, prep, /*originX*/10.0, /*originY*/20.0, tris2, lines2);
    CHECK(near(tris2[0].x, tris[0].x - 10.0f) && near(tris2[0].y, tris[0].y - 20.0f),
          "vertices are emitted relative to the scene origin");

    if (g_failures == 0) { std::printf("\nAll gpu_batches tests passed.\n"); return 0; }
    std::printf("\n%d gpu_batches test(s) failed.\n", g_failures);
    return 1;
}
