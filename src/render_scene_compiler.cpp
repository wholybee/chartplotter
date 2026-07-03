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
        // wash or an AP() pattern.
        const bool fill =
            f.kind == FeatureKind::DepthArea || f.kind == FeatureKind::LandArea ||
            (f.kind == FeatureKind::OtherArea &&
             (prep.hits[i].hasFill || prep.hits[i].apIndex >= 0));
        if (fill && !f.rings.empty() && f.rings[0].size() >= 3) {
            auto emitFill = [&prep, i](const std::vector<Pt>& poly) {
                std::vector<uint32_t> tris = geomtess::triangulate(poly);
                if (tris.empty()) return;
                PreparedFill pf;
                pf.featureIndex = static_cast<quint32>(i);
                pf.verts.reserve(poly.size() * 2);
                for (const Pt& p : poly) {
                    pf.verts.push_back(static_cast<float>(p.x));
                    pf.verts.push_back(static_cast<float>(p.y));
                }
                pf.indices.assign(tris.begin(), tris.end());
                prep.fills.push_back(std::move(pf));
            };

            if (f.rings.size() == 1) {
                emitFill(f.rings[0]);
            } else {
                // Extra rings inside the first are holes (bridged into it via
                // mergeHoles); rings outside it are detached outer polygons of
                // a multi-polygon feature, filled independently. This mirrors
                // the painter's even-odd fill for the common cases — an island
                // nested inside a hole is still treated as a hole (best
                // effort).
                std::vector<const std::vector<Pt>*> holes, outers;
                for (std::size_t r = 1; r < f.rings.size(); ++r) {
                    const std::vector<Pt>& ring = f.rings[r];
                    if (ring.size() < 3) continue;
                    if (geomtess::pointInRing(ring[0], f.rings[0]))
                        holes.push_back(&ring);
                    else
                        outers.push_back(&ring);
                }
                if (holes.empty())
                    emitFill(f.rings[0]);
                else
                    emitFill(geomtess::mergeHoles(f.rings[0], std::move(holes)));
                for (const std::vector<Pt>* o : outers) emitFill(*o);
            }
        }
    }
    return prep;
}

} // namespace scene
