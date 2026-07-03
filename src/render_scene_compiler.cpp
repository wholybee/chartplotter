// src/render_scene_compiler.cpp
#include "render_scene_compiler.hpp"
#include "sym_atlas.hpp"

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

        // GPU fill triangles are intentionally not prepared here. This stage is
        // view-independent, so it sees raw, unclipped cell polygons; triangulating
        // those dominated cold-cache CPU profiles. The GPU path now triangulates
        // the already clipped/simplified BuiltCell fill paths instead.
    }
    return prep;
}

} // namespace scene
