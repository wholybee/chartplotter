// src/chart_quilt.cpp
#include "chart_quilt.hpp"

#include <QPolygonF>
#include <QPointF>
#include <QRectF>
#include <algorithm>

namespace chartquilt {
namespace {

// Shift a box in X by dx (whole-world wrap offset).
BBox shiftX(const BBox& b, double dx) {
    BBox r = b;
    r.minx += dx; r.maxx += dx;
    return r;
}

// A cell's data-coverage footprint as a scene-frame QPainterPath, shifted into
// the common view frame by its wrap offset and clipped to `bound` (the kept
// region, also scene-frame). Scene Y is north-up, so y = -projected y. Falls
// back to the bbox rectangle when the cell has no M_COVR rings. Used by the
// quilting pass to decide, region by region, which band is the finest present.
QPainterPath coveragePath(const CellRecord& c, double off, const QPainterPath& bound) {
    QPainterPath pp;
    pp.setFillRule(Qt::WindingFill);
    if (!c.coverage.empty()) {
        for (const std::vector<Pt>& ring : c.coverage) {
            if (ring.size() < 3) continue;
            QPolygonF poly;
            poly.reserve(static_cast<int>(ring.size()));
            for (const Pt& p : ring) poly << QPointF(p.x + off, -p.y);
            pp.addPolygon(poly);
            pp.closeSubpath();
        }
    } else {
        const BBox& b = c.bbox;
        pp.addRect(QRectF(b.minx + off, -b.maxy, b.maxx - b.minx, b.maxy - b.miny));
    }
    return pp.intersected(bound);
}

} // namespace

QuiltResult computeQuilt(const std::vector<CellRecord>& cells,
                         const BBox& wantedArea, const BBox& keepArea,
                         int target,
                         const std::function<double(double)>& wrapOffsetFor) {
    QuiltResult out;

    // Which bands have coverage in view? (wrap-aware via per-cell offset.)
    bool present[kMaxBand + 1] = {};
    for (const CellRecord& c : cells) {
        if (!c.extentValid || c.band < 1 || c.band > kMaxBand) continue;
        const double off = wrapOffsetFor((c.bbox.minx + c.bbox.maxx) / 2.0);
        if (shiftX(c.bbox, off).intersects(wantedArea)) present[c.band] = true;
    }

    int maxBand = target;
    bool haveAtOrBelow = false;
    for (int b = 1; b <= target; ++b) if (present[b]) { haveAtOrBelow = true; break; }
    if (!haveAtOrBelow)
        for (int b = target + 1; b <= kMaxBand; ++b) if (present[b]) { maxBand = b; break; }
    out.maxBand = maxBand;

    // --- Quilting: only the finest band draws in any given region ------------
    // Walk candidate cells finest-band-first, accumulating a "covered" region.
    // A cell contributes only the part of its coverage that a finer band has not
    // already claimed; fully-covered cells are dropped, and partially-covered
    // cells get a clip path so their geometry and symbols draw only where they
    // are the finest data. Coarser bands then fill only the gaps — eliminating
    // the stacked-duplicate symbols that came from drawing every band.
    //
    // Region math runs in a common scene frame (each cell shifted by its wrap
    // offset into the view frame); the resulting clip is stored back in the
    // cell's own un-shifted frame so it composes with drawOffsetX when painted.
    // The work is bounded to keepArea (the 1.5x kept region) so the clips stay
    // valid for the whole loaded set, not just the tighter wanted area.
    const QRectF keepScene(keepArea.minx, -keepArea.maxy,
                           keepArea.maxx - keepArea.minx,
                           keepArea.maxy - keepArea.miny);
    QPainterPath keepClip;
    keepClip.addRect(keepScene);

    struct Cand { const CellRecord* rec; double off; };
    std::vector<Cand> cands;
    for (const CellRecord& c : cells) {
        if (!c.extentValid || c.band < 1 || c.band > maxBand) continue;
        const double off = wrapOffsetFor((c.bbox.minx + c.bbox.maxx) / 2.0);
        if (shiftX(c.bbox, off).intersects(keepArea)) cands.push_back({&c, off});
    }
    // Finest first: a higher band number is finer (harbour over coastal etc.).
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.rec->band > b.rec->band; });

    QSet<QString> newActive;
    QHash<QString, QPainterPath> newDrawClip;
    QPainterPath covered;                  // union of finer coverage, common frame
    covered.setFillRule(Qt::WindingFill);

    // Band-0 cells (filename didn't yield a usage band) can't be reasoned about,
    // so they always contribute, unclipped.
    for (const CellRecord& c : cells) {
        if (!c.extentValid || c.band != 0) continue;
        const double off = wrapOffsetFor((c.bbox.minx + c.bbox.maxx) / 2.0);
        if (shiftX(c.bbox, off).intersects(keepArea)) newActive.insert(c.path);
    }

    for (std::size_t i = 0; i < cands.size();) {
        // Process a whole band against the coverage of strictly-finer bands, so
        // same-band cells (which tile without overlap) never clip each other.
        const int band = cands[i].rec->band;
        const QPainterPath coveredByFiner = covered;
        QPainterPath bandUnion;
        bandUnion.setFillRule(Qt::WindingFill);
        std::size_t j = i;
        for (; j < cands.size() && cands[j].rec->band == band; ++j) {
            const CellRecord& c = *cands[j].rec;
            const double off = cands[j].off;
            const QPainterPath cov = coveragePath(c, off, keepClip);
            if (cov.isEmpty()) continue;
            bandUnion.addPath(cov);

            if (coveredByFiner.isEmpty() || !coveredByFiner.intersects(cov)) {
                newActive.insert(c.path);               // open: contributes, no clip
                continue;
            }
            const QPainterPath contrib = cov.subtracted(coveredByFiner);
            if (contrib.isEmpty()) continue;            // fully hidden: drop
            newActive.insert(c.path);
            newDrawClip.insert(c.path, contrib.translated(-off, 0.0));   // cell frame
        }
        covered.addPath(bandUnion);
        i = j;
    }

    out.active = std::move(newActive);
    out.drawClip = std::move(newDrawClip);

    // Wanted set = contributing cells whose footprint reaches the tighter wanted
    // area (the 0.5x margin that triggers loading, as before).
    for (const CellRecord& c : cells) {
        if (!c.extentValid || !out.active.contains(c.path)) continue;
        const double off = wrapOffsetFor((c.bbox.minx + c.bbox.maxx) / 2.0);
        if (shiftX(c.bbox, off).intersects(wantedArea)) out.wanted.insert(c.path);
    }

    return out;
}

} // namespace chartquilt
