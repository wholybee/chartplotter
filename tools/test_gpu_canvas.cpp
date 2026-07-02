// Stage 7 (path A): manual harness for the retained GPU chart renderer.
//
// Renders area-fill triangles + line geometry through Direct3D 11 with an
// interactive camera (drag to pan, wheel to zoom), in isolation from the app.
//
// Usage:
//   test_gpu_canvas [path-to-ENC-cell.000]
// If a cell path is given AND it has a valid parsed-cell cache entry (i.e. you
// have opened it in the app so its .pcell exists), its real geometry is drawn.
// Otherwise a synthetic chart-like scene is shown so the renderer can always be
// exercised. This proves GPU chart rendering + a retained pannable camera before
// the backend is integrated into ChartView.

#include "gpu_chart_view.hpp"
#include "prepared_chart_cache.hpp"
#include "chart_loader.hpp"
#include "geom_tessellate.hpp"

#include <QApplication>
#include <algorithm>
#include <vector>

namespace {

struct Rgb { float r, g, b; };

// Area fill colour, mirroring ChartView::fillColor (0-255 -> 0-1).
Rgb fillColor(const Feature& f) {
    auto c = [](int r, int g, int b) { return Rgb{ r / 255.f, g / 255.f, b / 255.f }; };
    if (f.kind == FeatureKind::LandArea) return c(217, 199, 148);
    if (f.kind == FeatureKind::OtherArea) return c(200, 200, 205);
    if (!f.hasDepth) return c(184, 212, 235);
    const double d = f.depth;
    if (d <  0.0) return c(158, 189, 140);
    if (d <  2.0) return c(102, 168, 217);
    if (d <  5.0) return c(143, 194, 227);
    if (d < 10.0) return c(184, 217, 240);
    if (d < 20.0) return c(217, 235, 250);
    return c(242, 247, 255);
}

Rgb lineColor(const Feature& f) {
    auto c = [](int r, int g, int b) { return Rgb{ r / 255.f, g / 255.f, b / 255.f }; };
    switch (f.kind) {
        case FeatureKind::Coastline:    return c(64, 51, 31);
        case FeatureKind::DepthContour: return c(115, 153, 199);
        default:                        return c(102, 102, 120);
    }
}

bool isAreaKind(FeatureKind k) {
    return k == FeatureKind::DepthArea || k == FeatureKind::LandArea ||
           k == FeatureKind::OtherArea;
}

// A tiny synthetic scene (metres) so the harness always shows something.
std::vector<Feature> syntheticScene() {
    std::vector<Feature> out;
    Feature water; water.kind = FeatureKind::DepthArea; water.hasDepth = true; water.depth = 8.0;
    water.rings = { { {-4000,-3000},{4000,-3000},{4000,3000},{-4000,3000} } };
    out.push_back(water);
    Feature land; land.kind = FeatureKind::LandArea;
    land.rings = { { {-3500,-1000},{-1500,-800},{-1000,1200},{-3000,1800},{-3800,600} } };
    out.push_back(land);
    Feature coast; coast.kind = FeatureKind::Coastline;
    coast.rings = { { {-1500,-800},{-1000,1200},{500,2000},{2500,1500} } };
    out.push_back(coast);
    for (Feature& f : out)
        for (auto& ring : f.rings)
            for (const Pt& p : ring) f.bbox.expand(p.x, p.y);
    return out;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    std::vector<Feature> feats;
    BBox bbox;
    bool real = false;
    if (argc > 1)
        real = prepared_cache::load(QString::fromLocal8Bit(argv[1]), feats, bbox);
    if (!real) {
        feats = syntheticScene();
        bbox = BBox{};
        for (const Feature& f : feats) bbox.expand(f.bbox);
    }
    if (!bbox.valid()) {
        qWarning("no geometry to draw");
        return 1;
    }

    const double ox = (bbox.minx + bbox.maxx) / 2.0;
    const double oy = (bbox.miny + bbox.maxy) / 2.0;

    std::vector<GpuVertex> tris, lines;
    for (const Feature& f : feats) {
        // Area fills: triangulate the exterior ring (holes ignored, as in the
        // prepared-render fills).
        if (isAreaKind(f.kind) && !f.rings.empty() && f.rings[0].size() >= 3) {
            const Rgb col = fillColor(f);
            const std::vector<Pt>& ring = f.rings[0];
            for (uint32_t idx : geomtess::triangulate(ring)) {
                const Pt& p = ring[idx];
                tris.push_back({ float(p.x - ox), float(p.y - oy), col.r, col.g, col.b });
            }
        }
        // Outlines / lines: every ring drawn as connected segments.
        const Rgb lc = lineColor(f);
        for (const auto& ring : f.rings) {
            if (ring.size() < 2) continue;
            const std::size_t n = ring.size();
            const bool closed = isAreaKind(f.kind);
            const std::size_t segs = closed ? n : n - 1;
            for (std::size_t i = 0; i < segs; ++i) {
                const Pt& a = ring[i];
                const Pt& b = ring[(i + 1) % n];
                lines.push_back({ float(a.x - ox), float(a.y - oy), lc.r, lc.g, lc.b });
                lines.push_back({ float(b.x - ox), float(b.y - oy), lc.r, lc.g, lc.b });
            }
        }
    }

    const double spanX = std::max(1.0, bbox.maxx - bbox.minx);
    const double spanY = std::max(1.0, bbox.maxy - bbox.miny);
    const double ppm = 0.9 * std::min(900.0 / spanX, 700.0 / spanY);

    GpuChartView w;
    w.setWindowTitle(real
        ? QStringLiteral("GPU chart — %1 features (D3D11; drag to pan, wheel to zoom)")
              .arg(feats.size())
        : QStringLiteral("GPU chart — synthetic scene (D3D11; drag to pan, wheel to zoom)"));
    w.setScene({}, {}, std::move(tris), std::move(lines), 0.0, 0.0, ppm);
    w.resize(900, 700);
    w.show();
    return app.exec();
}
