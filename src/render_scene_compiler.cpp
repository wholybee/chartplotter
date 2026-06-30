// src/render_scene_compiler.cpp
#include "render_scene_compiler.hpp"
#include "sym_atlas.hpp"
#include "geom_tessellate.hpp"

#include <QByteArray>

namespace scene {

PreparedCellRender compileScene(const QString& cellId,
                                const std::vector<Feature>& feats,
                                const SymAtlas* atlas) {
    PreparedCellRender prep;
    prep.formatVersion = kPreparedRenderFormat;
    prep.cellId = cellId;

    const std::size_t n = feats.size();
    prep.hits.resize(n);
    prep.hasHit.assign(n, 0);

    for (std::size_t i = 0; i < n; ++i) {
        const Feature& f = feats[i];

        // Portrayal. These rules mirror ChartView::instantiateCell exactly:
        // OtherArea/OtherLine resolve when an atlas + object class are present;
        // Points resolve whenever an atlas is loaded; nothing else is portrayed.
        if (f.kind == FeatureKind::OtherArea || f.kind == FeatureKind::OtherLine) {
            if (atlas && !f.objClass.empty()) {
                const SymGeom g = (f.kind == FeatureKind::OtherArea)
                                      ? SymGeom::Area : SymGeom::Line;
                prep.hits[i] = atlas->symbolForFeature(
                    QByteArray::fromStdString(f.objClass), g, f.attrs);
                prep.hasHit[i] = 1;
            }
        } else if (f.kind == FeatureKind::Point) {
            if (atlas) {
                prep.hits[i] = atlas->symbolForFeature(
                    QByteArray::fromStdString(f.objClass), SymGeom::Point, f.attrs);
                prep.hasHit[i] = 1;
            }
        }

        // Pre-triangulate area fills for the retained GPU backend (Stage 7). The
        // set of "fill" features matches instantiateCell's fillArea decision:
        // depth/land areas always fill; OtherArea fills when it carries an AC()
        // wash or an AP() pattern. Exterior ring only (holes deferred).
        const bool fill =
            f.kind == FeatureKind::DepthArea || f.kind == FeatureKind::LandArea ||
            (f.kind == FeatureKind::OtherArea &&
             (prep.hits[i].hasFill || prep.hits[i].apIndex >= 0));
        if (fill && !f.rings.empty() && f.rings[0].size() >= 3) {
            const std::vector<Pt>& ring = f.rings[0];
            std::vector<uint32_t> tris = geomtess::triangulate(ring);
            if (!tris.empty()) {
                PreparedFill pf;
                pf.featureIndex = static_cast<quint32>(i);
                pf.verts.reserve(ring.size() * 2);
                for (const Pt& p : ring) {
                    pf.verts.push_back(static_cast<float>(p.x));
                    pf.verts.push_back(static_cast<float>(p.y));
                }
                pf.indices.assign(tris.begin(), tris.end());
                prep.fills.push_back(std::move(pf));
            }
        }
    }
    return prep;
}

} // namespace scene
