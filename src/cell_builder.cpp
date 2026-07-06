// src/cell_builder.cpp
#include "cell_builder.hpp"
#include "geom_clip.hpp"

#include <QColor>
#include <QPainterPath>
#include <QPointF>

#include <algorithm>
#include <cmath>
#include <string>

using geom::clipRingToRect;
using geom::clipPolylineToRect;
using geom::pointInRect;

namespace {

// Builds a path in scene coordinates: projected X, and Y negated so north is up.
QPainterPath buildPathFromRings(const std::vector<std::vector<Pt>>& rings, bool closed) {
    QPainterPath path;
    if (closed) path.setFillRule(Qt::OddEvenFill);
    for (const auto& ring : rings) {
        if (ring.size() < 2) continue;
        path.moveTo(ring[0].x, -ring[0].y);
        for (std::size_t i = 1; i < ring.size(); ++i)
            path.lineTo(ring[i].x, -ring[i].y);
        if (closed) path.closeSubpath();
    }
    return path;
}

// Area-weighted centroid of a polygon ring (projected coords). Falls back to
// the vertex average for a degenerate (near-zero-area) ring. Used to place an
// area feature's centred symbol (e.g. an anchorage's anchor glyph).
Pt ringCentroid(const std::vector<Pt>& ring) {
    const std::size_t n = ring.size();
    if (n == 0) return {0.0, 0.0};
    double a = 0.0, cx = 0.0, cy = 0.0;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const double cross = ring[j].x * ring[i].y - ring[i].x * ring[j].y;
        a  += cross;
        cx += (ring[j].x + ring[i].x) * cross;
        cy += (ring[j].y + ring[i].y) * cross;
    }
    if (std::abs(a) < 1e-9) {
        double sx = 0.0, sy = 0.0;
        for (const Pt& p : ring) { sx += p.x; sy += p.y; }
        return { sx / static_cast<double>(n), sy / static_cast<double>(n) };
    }
    a *= 0.5;
    return { cx / (6.0 * a), cy / (6.0 * a) };
}

} // namespace

namespace cellbuilder {

QColor fillColor(const Feature& f) {
    if (f.kind == FeatureKind::LandArea) return QColor(217, 199, 148);
    if (!f.hasDepth)                     return QColor(184, 212, 235);
    double d = f.depth;
    if (d <  0.0)  return QColor(158, 189, 140);
    if (d <  2.0)  return QColor(102, 168, 217);
    if (d <  5.0)  return QColor(143, 194, 227);
    if (d < 10.0)  return QColor(184, 217, 240);
    if (d < 20.0)  return QColor(217, 235, 250);
    return QColor(242, 247, 255);
}

BuiltCell instantiateCell(const QString& path, const std::vector<Feature>& feats,
                          const PreparedCellRender& prep,
                          int band, const BBox& clipBox, double tol) {
    BuiltCell bc;
    bc.path    = path;
    bc.band    = band;
    bc.clipBox = clipBox;

    const double zb = static_cast<double>(band) * 1000.0;

    // Portrayal result for feature i (default-empty when none was resolved).
    auto hitAt = [&prep](std::size_t i) -> const SymHit& {
        static const SymHit kEmpty;
        return i < prep.hits.size() ? prep.hits[i] : kEmpty;
    };

    std::vector<std::vector<Pt>> clipBuf;
    std::vector<std::vector<Pt>> simpBuf;

    auto reduce = [&](const std::vector<std::vector<Pt>>& src, std::size_t minPts,
                      bool closed) -> const std::vector<std::vector<Pt>>& {
        if (tol <= 0.0) return src;
        simpBuf.clear();
        for (const auto& ring : src) {
            // Closed area rings use the ring-aware simplifier so aggressive
            // zoom-out tolerances can't collapse or self-intersect them; open
            // polylines keep the plain endpoint-anchored Douglas–Peucker.
            std::vector<Pt> s = closed ? geom::simplifyRing(ring, tol)
                                       : geom::simplify(ring, tol);
            if (s.size() >= minPts) simpBuf.push_back(std::move(s));
        }
        return simpBuf;
    };

    // Emit the TX()/TE() labels a SymHit produced at scene position `pos`. When
    // the instruction carried no text but the feature has an OBJNAM, fall back
    // to a single name label (the pre-S-52-text behaviour) so named objects keep
    // their label.
    auto pushHitTexts = [&](const SymHit& hit, QPointF pos, int scaleMin,
                            const std::string& fallbackName) {
        if (!hit.texts.empty()) {
            for (const SymText& t : hit.texts) {
                BuiltText bt;
                bt.pos = pos; bt.text = t.text; bt.scaleMin = scaleMin;
                bt.hjust = t.hjust; bt.vjust = t.vjust;
                bt.xoffs = t.xoffs; bt.yoffs = t.yoffs;
                bt.color = QColor(t.r, t.g, t.b);
                bt.pointSize = t.pointSize;
                bc.texts.push_back(std::move(bt));
            }
        } else if (!fallbackName.empty()) {
            BuiltText bt;
            bt.pos = pos; bt.text = QString::fromStdString(fallbackName);
            bt.scaleMin = scaleMin;
            bc.texts.push_back(std::move(bt));
        }
    };

    for (std::size_t i = 0; i < feats.size(); ++i) {
        const Feature& f = feats[i];
        const bool doClip = clipBox.valid() && !clipBox.contains(f.bbox);

        switch (f.kind) {
            case FeatureKind::DepthArea:
            case FeatureKind::LandArea: {
                const std::vector<std::vector<Pt>>* rings = &f.rings;
                if (doClip) {
                    clipBuf.clear();
                    for (const auto& ring : f.rings) {
                        std::vector<Pt> c = clipRingToRect(ring, clipBox);
                        if (c.size() >= 3) clipBuf.push_back(std::move(c));
                    }
                    if (clipBuf.empty()) break;
                    rings = &clipBuf;
                }
                const std::vector<std::vector<Pt>>& use = reduce(*rings, 3, /*closed=*/true);
                if (use.empty()) break;

                BuiltPath bp;
                bp.path   = buildPathFromRings(use, true);
                bp.bounds = bp.path.boundingRect();
                bp.z      = zb + f.zorder;
                bp.filled = true;
                bp.brush  = fillColor(f);
                if (f.kind == FeatureKind::LandArea) {
                    bp.hasPen = true; bp.penColor = QColor(115, 97, 64); bp.penWidth = 1.0;
                }
                bc.paths.push_back(std::move(bp));
                break;
            }
            case FeatureKind::OtherArea:
            case FeatureKind::DepthContour:
            case FeatureKind::Coastline:
            case FeatureKind::OtherLine: {
                // S-52 lookup for OtherArea / OtherLine, resolved at prepare time
                // (scene::compileScene) and read back here. For OtherArea: any
                // AC() fills the polygon, LS() styles the boundary, and any SY()
                // drops at the centroid. For OtherLine: LS() styles the line.
                // hitAt(i) is default-empty for non-symbol-bearing kinds
                // (DepthContour/Coastline) and when no atlas was loaded.
                const SymHit& hit = hitAt(i);

                // When the area carries an AC() fill or an AP() pattern we need
                // closed polygons (Sutherland-Hodgman ring clip + closeSubpath);
                // otherwise the existing polyline clip is correct for outlines.
                const bool fillArea = (f.kind == FeatureKind::OtherArea) &&
                                      (hit.hasFill || hit.apIndex >= 0);

                const std::vector<std::vector<Pt>>* rings = &f.rings;
                if (doClip) {
                    clipBuf.clear();
                    if (fillArea) {
                        for (const auto& ring : f.rings) {
                            std::vector<Pt> c = clipRingToRect(ring, clipBox);
                            if (c.size() >= 3) clipBuf.push_back(std::move(c));
                        }
                    } else {
                        for (const auto& ring : f.rings) {
                            std::vector<std::vector<Pt>> runs = clipPolylineToRect(ring, clipBox);
                            for (auto& run : runs) clipBuf.push_back(std::move(run));
                        }
                    }
                    if (clipBuf.empty()) break;
                    rings = &clipBuf;
                }
                const std::vector<std::vector<Pt>>& use =
                    reduce(*rings, fillArea ? 3 : 2, /*closed=*/fillArea);
                if (use.empty()) break;

                BuiltPath bp;
                bp.path    = buildPathFromRings(use, fillArea);
                bp.bounds  = bp.path.boundingRect();
                bp.z       = zb + f.zorder;
                bp.filled  = hit.hasFill;   // AC() wash (AP pattern overlays it)
                bp.apIndex = (f.kind == FeatureKind::OtherArea) ? hit.apIndex : -1;
                bp.lcIndex = hit.lcIndex;
                bp.scaleMin = f.scaleMin;   // SCAMIN floor for the LC/AP overlay pass
                if (hit.hasFill)
                    bp.brush = QColor(hit.fill.r, hit.fill.g, hit.fill.b, hit.fill.a);
                bp.hasPen = true;
                if (hit.hasLine) {
                    bp.penColor = QColor(hit.line.r, hit.line.g, hit.line.b);
                    bp.penWidth = static_cast<qreal>(hit.line.width);
                    bp.penStyle = (hit.line.pattern == SymLineStyle::Dash) ? Qt::DashLine
                                : (hit.line.pattern == SymLineStyle::Dot)  ? Qt::DotLine
                                : Qt::SolidLine;
                }
                // A complex line (LC) replaces the plain outline; keep just a
                // faint guide so the boundary still reads if the motif is sparse.
                else if (hit.lcIndex >= 0)                        { bp.penColor = QColor(120, 120, 130, 90);  bp.penWidth = 0.6; }
                else if (hit.hasFill && bp.apIndex < 0)           { bp.hasPen = false; }   // fill-only area
                else if (f.kind == FeatureKind::Coastline)        { bp.penColor = QColor(64, 51, 31);         bp.penWidth = 1.4; }
                else if (f.kind == FeatureKind::DepthContour)     { bp.penColor = QColor(115, 153, 199);      bp.penWidth = 0.8; }
                else if (f.kind == FeatureKind::OtherArea)        { bp.penColor = QColor(102, 102, 115, 150); bp.penWidth = 0.7; }
                else                                              { bp.penColor = QColor(102, 102, 128);      bp.penWidth = 0.8; }
                bp.isDepthContour = (f.kind == FeatureKind::DepthContour);
                bc.paths.push_back(std::move(bp));

                // Area centred symbols (e.g. ACHARE anchor glyph, TSSLPT
                // direction arrow, restriction glyph from CS(RESTRN01)) and text
                // labels, placed at the polygon centroid so they stay put as the
                // viewport pans. Computed from the unclipped outer ring.
                if (f.kind == FeatureKind::OtherArea &&
                    !f.rings.empty() && !f.rings[0].empty()) {
                    const Pt c = ringCentroid(f.rings[0]);
                    const QPointF cp(c.x, -c.y);
                    for (const SymStamp& s : hit.symbols) {
                        if (s.symIdx == kNoSymbol) continue;
                        BuiltSymbol bs;
                        bs.pos = cp; bs.symIdx = s.symIdx;
                        bs.rotationDeg = s.rotationDeg; bs.scaleMin = f.scaleMin;
                        bc.symbols.push_back(bs);
                    }
                    pushHitTexts(hit, cp, f.scaleMin, f.name);
                }
                break;
            }
            case FeatureKind::Sounding: {
                if (f.rings.empty() || f.rings[0].empty()) break;
                if (doClip && !pointInRect(f.rings[0][0], clipBox)) break;
                // Keep the raw depth (metres); the label is formatted at paint
                // time so the depth-unit preference is a repaint, not a rebuild.
                bc.soundings.push_back(
                    { QPointF(f.rings[0][0].x, -f.rings[0][0].y), f.depth, f.hasDepth,
                      f.scaleMin });
                break;
            }
            case FeatureKind::Point: {
                if (f.rings.empty() || f.rings[0].empty()) break;
                if (doClip && !pointInRect(f.rings[0][0], clipBox)) break;
                const QPointF pos(f.rings[0][0].x, -f.rings[0][0].y);
                const SymHit& hit = hitAt(i);
                // One BuiltSymbol per SY() stamp (lights add a flare, buoys add
                // a topmark, etc.). With no atlas / no resolved symbol, emit a
                // single dot-fallback marker.
                if (hit.symbols.empty()) {
                    BuiltSymbol bs; bs.pos = pos; bs.scaleMin = f.scaleMin;
                    bc.symbols.push_back(bs);
                } else {
                    for (const SymStamp& s : hit.symbols) {
                        BuiltSymbol bs;
                        bs.pos = pos; bs.symIdx = s.symIdx;
                        bs.rotationDeg = s.rotationDeg; bs.scaleMin = f.scaleMin;
                        bc.symbols.push_back(bs);
                    }
                }
                // Light-sector arcs (one per sectored LIGHTS feature), anchored at
                // the light's scene position so they pan with the chart.
                for (const SymSector& s : hit.sectors) {
                    BuiltLightSector ls;
                    ls.pos = pos;
                    ls.startDeg = s.startDeg; ls.endDeg = s.endDeg;
                    ls.rangeNm = s.rangeNm; ls.scaleMin = f.scaleMin;
                    ls.color = QColor(s.r, s.g, s.b);
                    bc.sectors.push_back(ls);
                }
                pushHitTexts(hit, pos, f.scaleMin, f.name);
                break;
            }
        }
    }

    std::sort(bc.paths.begin(), bc.paths.end(),
              [](const BuiltPath& a, const BuiltPath& b) { return a.z < b.z; });
    return bc;
}

} // namespace cellbuilder
