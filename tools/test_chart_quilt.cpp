// tools/test_chart_quilt.cpp
//
// Unit test for the pure quilt selection extracted in Stage 7 A3
// (src/chart_quilt.cpp). Verifies the rules the painter (and the coming GPU
// backend) depend on: finest band wins a region, coarser bands are dropped or
// clipped to the gaps, band-0 always contributes, and the wanted set tracks the
// tight area. No Qt event loop or GDAL — just value logic.

#include "chart_quilt.hpp"
#include "chart_catalog.hpp"
#include "chart_loader.hpp"

#include <cstdio>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) { std::printf("FAIL: %s\n", (msg)); ++g_failures; }        \
        else         { std::printf("ok:   %s\n", (msg)); }                      \
    } while (0)

// A cell whose coverage is exactly its bbox rectangle (no explicit M_COVR),
// which is the common case coveragePath falls back to.
static CellRecord makeCell(const char* path, int band,
                           double minx, double miny, double maxx, double maxy) {
    CellRecord c;
    c.path = QString::fromLatin1(path);
    c.band = band;
    c.bbox = BBox{minx, miny, maxx, maxy};
    c.extentValid = true;
    return c;
}

int main() {
    // No wrap: the whole scene sits far from any 180° seam, so every cell's
    // offset is 0.
    auto noWrap = [](double) { return 0.0; };

    // ---- Case 1: finer band fully covers a coarser one over the same area ----
    // A coastal (band 3) cell and a harbour (band 5) cell over the identical
    // footprint. The harbour is finer, so it draws; the coastal is fully covered
    // and must be dropped from active (not merely clipped).
    {
        std::vector<CellRecord> cells = {
            makeCell("coastal", 3, 0, 0, 100, 100),
            makeCell("harbour", 5, 0, 0, 100, 100),
        };
        const BBox wanted{-50, -50, 150, 150};
        const BBox keep  {-50, -50, 150, 150};
        const chartquilt::QuiltResult q =
            chartquilt::computeQuilt(cells, wanted, keep, /*target*/5, noWrap);

        CHECK(q.maxBand == 5, "case1: maxBand is the finest present (5)");
        CHECK(q.active.contains("harbour"), "case1: harbour active");
        CHECK(!q.active.contains("coastal"), "case1: coastal fully covered -> dropped");
        CHECK(q.wanted.contains("harbour"), "case1: harbour wanted");
    }

    // ---- Case 2: partial overlap -> coarser cell survives, clipped -----------
    // The harbour covers only the left half of the coastal cell. The coastal
    // stays active but with a draw-clip (it draws only where it is the finest).
    {
        std::vector<CellRecord> cells = {
            makeCell("coastal", 3,  0, 0, 100, 100),
            makeCell("harbour", 5,  0, 0,  50, 100),   // left half only
        };
        const BBox wanted{-10, -10, 110, 110};
        const BBox keep  {-10, -10, 110, 110};
        const chartquilt::QuiltResult q =
            chartquilt::computeQuilt(cells, wanted, keep, /*target*/5, noWrap);

        CHECK(q.active.contains("harbour"), "case2: harbour active");
        CHECK(q.active.contains("coastal"), "case2: coastal still active (gap on the right)");
        CHECK(q.drawClip.contains("coastal"), "case2: coastal is clipped to the gap");
        CHECK(!q.drawClip.contains("harbour"), "case2: harbour unclipped (finest)");
        CHECK(!q.drawClip.value("coastal").isEmpty(), "case2: coastal clip path is non-empty");
    }

    // ---- Case 3: band-0 cells always contribute, unclipped -------------------
    {
        std::vector<CellRecord> cells = {
            makeCell("unknown", 0, 0, 0, 100, 100),
            makeCell("harbour", 5, 0, 0, 100, 100),
        };
        const BBox wanted{-10, -10, 110, 110};
        const BBox keep  {-10, -10, 110, 110};
        const chartquilt::QuiltResult q =
            chartquilt::computeQuilt(cells, wanted, keep, /*target*/5, noWrap);

        CHECK(q.active.contains("unknown"), "case3: band-0 cell always active");
        CHECK(!q.drawClip.contains("unknown"), "case3: band-0 cell never clipped");
    }

    // ---- Case 4: wanted tracks the tight area, keep is wider -----------------
    // The cell reaches keepArea but not the tighter wantedArea, so it is active
    // (kept/loaded) yet not "wanted" (would not trigger a fresh load pull-in).
    {
        std::vector<CellRecord> cells = {
            makeCell("far", 4, 200, 200, 300, 300),
        };
        const BBox wanted{0, 0, 100, 100};      // does not reach the cell
        const BBox keep  {0, 0, 400, 400};      // does reach the cell
        const chartquilt::QuiltResult q =
            chartquilt::computeQuilt(cells, wanted, keep, /*target*/4, noWrap);

        CHECK(q.active.contains("far"), "case4: cell within keepArea is active");
        CHECK(!q.wanted.contains("far"), "case4: cell outside wantedArea is not wanted");
    }

    if (g_failures == 0) { std::printf("\nAll quilt tests passed.\n"); return 0; }
    std::printf("\n%d quilt test(s) failed.\n", g_failures);
    return 1;
}
