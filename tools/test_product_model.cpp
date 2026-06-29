// Stage 3 round-trip test: a legacy S-57 Feature vector converted into the
// normalized product model and back must be identical, field for field. This is
// the data-level guarantee behind "S-57 renders unchanged through the
// compatibility adapter". No GDAL: features are synthetic, so the test is fast
// and hermetic.
//
// Plain console executable, like gen_symbols. Returns 0 on success, 1 on the
// first failing assertion (with a diagnostic on stderr).

#include "product_adapter.hpp"
#include "chart_loader.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL: %s  [%s:%d]\n", (msg), __FILE__,       \
                         __LINE__);                                            \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

bool ringsEqual(const std::vector<std::vector<Pt>>& a,
                const std::vector<std::vector<Pt>>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].size() != b[i].size()) return false;
        for (std::size_t j = 0; j < a[i].size(); ++j)
            if (a[i][j].x != b[i][j].x || a[i][j].y != b[i][j].y) return false;
    }
    return true;
}

void checkFeatureEqual(const Feature& a, const Feature& b, const char* tag) {
    CHECK(a.kind == b.kind, tag);
    CHECK(a.zorder == b.zorder, tag);
    CHECK(a.depth == b.depth, tag);
    CHECK(a.hasDepth == b.hasDepth, tag);
    CHECK(a.scaleMin == b.scaleMin, tag);
    CHECK(a.objClass == b.objClass, tag);
    CHECK(a.attrs == b.attrs, tag);
    CHECK(a.name == b.name, tag);
    CHECK(a.bbox.minx == b.bbox.minx && a.bbox.miny == b.bbox.miny &&
          a.bbox.maxx == b.bbox.maxx && a.bbox.maxy == b.bbox.maxy, tag);
    CHECK(ringsEqual(a.rings, b.rings), tag);
}

Feature makeArea() {
    Feature f;
    f.kind = FeatureKind::DepthArea;
    f.zorder = 0;
    f.depth = 12.5;
    f.hasDepth = true;
    f.scaleMin = 45000;
    f.rings = {{{0, 0}, {100, 0}, {100, 100}, {0, 100}},   // exterior
               {{25, 25}, {75, 25}, {50, 75}}};            // hole
    for (const auto& ring : f.rings)
        for (const Pt& p : ring) f.bbox.expand(p.x, p.y);
    // DepthArea is not symbol-bearing: no objClass / attrs / name.
    return f;
}

Feature makePoint() {
    Feature f;
    f.kind = FeatureKind::Point;
    f.zorder = 40;
    f.objClass = "BOYLAT";
    f.attrs = {{"BOYSHP", "2"}, {"COLOUR", "1,4"}};
    f.name = "No. 3";
    f.scaleMin = 0;
    f.rings = {{{-123.0, 49.0}}};
    f.bbox.expand(-123.0, 49.0);
    return f;
}

Feature makeLine() {
    Feature f;
    f.kind = FeatureKind::Coastline;   // not symbol-bearing
    f.zorder = 22;
    f.rings = {{{0, 0}, {10, 5}, {20, 7}, {30, 2}}};
    for (const Pt& p : f.rings[0]) f.bbox.expand(p.x, p.y);
    return f;
}

Feature makeStyledLine() {
    Feature f;
    f.kind = FeatureKind::OtherLine;   // symbol-bearing (carries class + attrs)
    f.zorder = 20;
    f.objClass = "TSSBND";
    f.attrs = {{"CATTSS", "1"}};
    f.rings = {{{1, 1}, {2, 2}, {3, 1}}};
    for (const Pt& p : f.rings[0]) f.bbox.expand(p.x, p.y);
    return f;
}

} // namespace

int main() {
    std::vector<Feature> original;
    original.push_back(makeArea());
    original.push_back(makePoint());
    original.push_back(makeLine());
    original.push_back(makeStyledLine());

    // Keep an independent copy to compare against after the round trip.
    const std::vector<Feature> reference = original;

    const QString cellId = QStringLiteral("/charts/US5WA22M.000");
    ProductFeatureSet pfs =
        product_adapter::fromLegacyFeatures(std::move(original), cellId);

    // Normalized-model spot checks.
    CHECK(pfs.cell.cellId == cellId, "cell id preserved");
    CHECK(pfs.cell.product.product == QStringLiteral("S-57"), "product id");
    CHECK(pfs.features.size() == reference.size(), "feature count");
    CHECK(pfs.features[1].classId.code == QStringLiteral("S57:BOYLAT"),
          "namespaced class code");
    CHECK(pfs.features[1].attrs.size() == 2 &&
          pfs.features[1].attrs[0].id == QStringLiteral("S57:BOYSHP"),
          "namespaced attribute id");
    CHECK(pfs.features[0].classId.code.isEmpty(),
          "non-symbol feature has empty class code");
    CHECK(pfs.features[1].stableId != pfs.features[0].stableId,
          "distinct stable ids");
    // Geometry pooled: 2 (area) + 1 (point) + 1 (line) + 1 (styled line) = 5.
    CHECK(pfs.geometry.rings.size() == 5, "rings pooled in geometry store");

    BBox bbox;
    std::vector<Feature> restored =
        product_adapter::toLegacyFeatures(std::move(pfs), bbox);

    CHECK(restored.size() == reference.size(), "restored feature count");
    const char* tags[] = {"area", "point", "line", "styled-line"};
    for (std::size_t i = 0; i < restored.size() && i < reference.size(); ++i)
        checkFeatureEqual(restored[i], reference[i], tags[i]);

    if (g_failures == 0) {
        std::printf("product model round-trip: OK (%zu features)\n",
                    reference.size());
        return 0;
    }
    std::fprintf(stderr, "product model round-trip: %d failure(s)\n", g_failures);
    return 1;
}
