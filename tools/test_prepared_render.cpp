// Stage 5 tests: the ear-clip tessellator and the prepared-render cache
// serialization round trip. Hermetic (QStandardPaths test mode); no GUI.
// Returns 0 on success, 1 on the first failing assertion.

#include "prepared_render.hpp"
#include "prepared_render_cache.hpp"
#include "geom_tessellate.hpp"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL: %s  [%s:%d]\n", (msg), __FILE__,        \
                         __LINE__);                                             \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

// Sum of unsigned triangle areas for an index triangulation of `ring`.
double triangulatedArea(const std::vector<Pt>& ring,
                        const std::vector<uint32_t>& idx) {
    double area = 0.0;
    for (std::size_t k = 0; k + 2 < idx.size(); k += 3) {
        const Pt& a = ring[idx[k]];
        const Pt& b = ring[idx[k + 1]];
        const Pt& c = ring[idx[k + 2]];
        area += std::fabs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) * 0.5;
    }
    return area;
}

void testTessellation() {
    // Square -> 2 triangles, total area 100.
    const std::vector<Pt> square = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
    const auto sq = geomtess::triangulate(square);
    CHECK(sq.size() == 6, "square triangulates to 2 triangles");
    CHECK(std::fabs(triangulatedArea(square, sq) - 100.0) < 1e-6,
          "square triangle area sums to 100");

    // Concave L-shape (6 verts) -> 4 triangles, total area 12.
    const std::vector<Pt> L = {{0, 0}, {4, 0}, {4, 2}, {2, 2}, {2, 4}, {0, 4}};
    const auto tl = geomtess::triangulate(L);
    CHECK(tl.size() == 12, "L-shape triangulates to 4 triangles");
    CHECK(std::fabs(triangulatedArea(L, tl) - 12.0) < 1e-6,
          "L-shape triangle area sums to 12");

    // Clockwise square should also work (orientation-independent).
    const std::vector<Pt> cw = {{0, 0}, {0, 10}, {10, 10}, {10, 0}};
    const auto cwt = geomtess::triangulate(cw);
    CHECK(cwt.size() == 6, "clockwise square triangulates to 2 triangles");
    CHECK(std::fabs(triangulatedArea(cw, cwt) - 100.0) < 1e-6,
          "clockwise square area sums to 100");

    CHECK(geomtess::triangulate({{0, 0}, {1, 1}}).empty(),
          "degenerate ring yields no triangles");
}

void testCacheRoundTrip() {
    QTemporaryFile src;
    CHECK(src.open(), "temp source file opens");
    src.write("dummy chart cell");
    src.flush();
    const QString srcPath = src.fileName();
    const quint64 fp = 0xABCDEF0123456789ull;

    PreparedCellRender prep;
    prep.formatVersion = 1;
    prep.cellId = QStringLiteral("US5WA22M");
    prep.hits.resize(2);
    prep.hasHit = {0, 1};

    SymHit h;
    h.symbols.push_back(SymStamp{42, 90.0f});
    h.hasLine = true;
    h.line = SymLineStyle{SymLineStyle::Dash, 3, 1, 2, 3};
    h.hasFill = true;
    h.fill = SymFillStyle{10, 20, 30, 40};
    h.lcIndex = 5;
    h.apIndex = 7;
    SymText t;
    t.text = QStringLiteral("Fl R 5s");
    t.hjust = 3; t.vjust = 2; t.space = 2; t.xoffs = 1; t.yoffs = -2;
    t.r = 4; t.g = 5; t.b = 6; t.pointSize = 9;
    h.texts.push_back(t);
    SymSector sec;
    sec.startDeg = 10.0f; sec.endDeg = 80.0f; sec.rangeNm = 12.0f;
    sec.r = 255; sec.g = 0; sec.b = 0;
    h.sectors.push_back(sec);
    prep.hits[1] = h;

    PreparedFill pf;
    pf.featureIndex = 1;
    pf.verts = {0.f, 0.f, 10.f, 0.f, 10.f, 10.f};
    pf.indices = {0, 1, 2};
    prep.fills.push_back(pf);

    CHECK(prepared_render_cache::store(srcPath, fp, prep), "store succeeds");

    PreparedCellRender out;
    CHECK(prepared_render_cache::load(srcPath, fp, out), "load succeeds");
    CHECK(out.hits.size() == 2 && out.hasHit.size() == 2 &&
          out.hasHit[0] == 0 && out.hasHit[1] == 1, "feature counts + hasHit");

    const SymHit& g = out.hits[1];
    CHECK(g.symbols.size() == 1 && g.symbols[0].symIdx == 42 &&
          g.symbols[0].rotationDeg == 90.0f, "symbol round-trips");
    CHECK(g.hasLine && g.line.pattern == SymLineStyle::Dash &&
          g.line.width == 3 && g.line.r == 1 && g.line.g == 2 && g.line.b == 3,
          "line style round-trips");
    CHECK(g.hasFill && g.fill.r == 10 && g.fill.a == 40, "fill round-trips");
    CHECK(g.lcIndex == 5 && g.apIndex == 7, "lc/ap indices round-trip");
    CHECK(g.texts.size() == 1 && g.texts[0].text == QStringLiteral("Fl R 5s") &&
          g.texts[0].hjust == 3 && g.texts[0].xoffs == 1 &&
          g.texts[0].yoffs == -2 && g.texts[0].pointSize == 9,
          "text round-trips");
    CHECK(g.sectors.size() == 1 && g.sectors[0].startDeg == 10.0f &&
          g.sectors[0].endDeg == 80.0f && g.sectors[0].rangeNm == 12.0f &&
          g.sectors[0].r == 255, "sector round-trips");
    CHECK(out.fills.size() == 1 && out.fills[0].featureIndex == 1 &&
          out.fills[0].verts.size() == 6 && out.fills[0].indices.size() == 3 &&
          out.fills[0].verts[2] == 10.0f && out.fills[0].indices[2] == 2,
          "fill round-trips");

    // A different portrayal fingerprint must be a miss (invalidation).
    PreparedCellRender miss;
    CHECK(!prepared_render_cache::load(srcPath, fp + 1, miss),
          "fingerprint mismatch is a miss");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("test"));
    QCoreApplication::setApplicationName(QStringLiteral("prepared_render_test"));
    QStandardPaths::setTestModeEnabled(true);

    testTessellation();
    testCacheRoundTrip();

    if (g_failures == 0) {
        std::printf("prepared render: OK\n");
        return 0;
    }
    std::fprintf(stderr, "prepared render: %d failure(s)\n", g_failures);
    return 1;
}
