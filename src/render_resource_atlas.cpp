// src/render_resource_atlas.cpp
#include "render_resource_atlas.hpp"

#include <QTransform>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstring>

// HPGL units -> pixels.  Calibrated so vector motifs render at roughly the same
// on-screen size as the prebaked point-symbol bitmaps (~0.034-0.038 px/unit).
static constexpr double kHpglToPx = 0.036;

namespace {

// Parse a comma list of numbers (HPGL coordinate args).
std::vector<double> parseNums(const QByteArray& s) {
    std::vector<double> out;
    for (const QByteArray& p : s.split(',')) {
        const QByteArray t = p.trimmed();
        if (!t.isEmpty()) out.push_back(t.toDouble());
    }
    return out;
}

} // namespace

// ---- load -------------------------------------------------------------------

bool RenderResourceAtlas::load(const BinSymRecord* symRecs, uint32_t symCount,
                               const BinLcDefRecord* lcRecs, uint32_t lcCount,
                               const BinApDefRecord* apRecs, uint32_t apCount,
                               const char* strBase, std::size_t strBytes,
                               const QPixmap& pm) {
    if (pm.isNull()) return false;

    // Symbols.
    rects_.resize(symCount);
    pivots_.resize(symCount);
    nameIndex_.reserve(static_cast<int>(symCount));
    for (uint32_t i = 0; i < symCount; ++i) {
        const BinSymRecord& s = symRecs[i];
        rects_[i]  = QRect(s.atlas_x, s.atlas_y, s.width, s.height);
        pivots_[i] = QPoint(s.pivot_x, s.pivot_y);
        nameIndex_[QByteArray(s.name)] = static_cast<uint16_t>(i);
    }

    // Compile LC line-complex definitions.
    lcDefs_.resize(lcCount);
    for (uint32_t i = 0; i < lcCount; ++i) {
        const BinLcDefRecord& d = lcRecs[i];
        LcDef def;
        def.color = QColor(d.r, d.g, d.b, d.a);
        def.advance = d.vecW > 0 ? double(d.vecW) : 1.0;
        def.pivot = QPointF(d.pivotX, d.pivotY);
        if (d.hpglLen && std::size_t(d.hpglOff) + d.hpglLen <= strBytes)
            def.strokes = compileHpgl(QByteArray(strBase + d.hpglOff, d.hpglLen));
        // How far the motif's drawn content reaches to the right of the pivot, so
        // the last stamp on a line can be dropped before it overshoots the end.
        QRectF bb;
        for (const auto& s : def.strokes)
            bb = bb.isNull() ? s.path.boundingRect() : bb.united(s.path.boundingRect());
        def.reach = bb.isNull() ? def.advance
                                : std::max(def.advance, bb.right() - def.pivot.x());
        lcDefs_[i] = std::move(def);
        lcIndex_.insert(QByteArray(d.name), static_cast<int>(i));
    }

    // Build AP area-pattern tiles (raster copy or HPGL render).
    apDefs_.resize(apCount);
    for (uint32_t i = 0; i < apCount; ++i) {
        const BinApDefRecord& d = apRecs[i];
        ApDef def;
        def.staggered = (d.fillType == 0);   // 0=staggered(S), 1=linear(L)

        QImage tile;
        if (d.hpglLen && std::size_t(d.hpglOff) + d.hpglLen <= strBytes) {
            // Render the HPGL motif into a transparent tile sized to its bbox.
            const auto strokes = compileHpgl(QByteArray(strBase + d.hpglOff, d.hpglLen));
            QRectF bb;
            for (const auto& s : strokes)
                bb = bb.isNull() ? s.path.boundingRect()
                                 : bb.united(s.path.boundingRect());
            if (!bb.isEmpty()) {
                const int w = std::max(1, int(std::ceil(bb.width()  * kHpglToPx)) + 2);
                const int h = std::max(1, int(std::ceil(bb.height() * kHpglToPx)) + 2);
                tile = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
                tile.fill(Qt::transparent);
                QPainter tp(&tile);
                tp.setRenderHint(QPainter::Antialiasing, true);
                tp.translate(1, 1);
                tp.scale(kHpglToPx, kHpglToPx);
                tp.translate(-bb.left(), -bb.top());
                const QColor col(d.r, d.g, d.b, d.a);
                for (const auto& s : strokes) {
                    if (s.fill) { tp.setPen(Qt::NoPen); tp.setBrush(col); tp.drawPath(s.path); }
                    else {
                        QPen pen(col); pen.setCosmetic(true);
                        pen.setWidthF(std::max(1.0, s.width)); tp.setPen(pen);
                        tp.setBrush(Qt::NoBrush); tp.drawPath(s.path);
                    }
                }
                tp.end();
            }
        } else if (d.hasBitmap && d.bmpW > 0 && d.bmpH > 0) {
            // Raster pattern: copy the tile straight out of the atlas.
            tile = pm.copy(QRect(d.bmpX, d.bmpY, d.bmpW, d.bmpH)).toImage();
        }
        def.tile = tile;

        // Spacing between motif anchors. Use the S-52 <distance> min when given,
        // otherwise the motif's own size so the pattern tiles edge-to-edge.
        const double motif = std::max(tile.width(), tile.height());
        def.spacing = (d.minDist > 0) ? std::max(motif, d.minDist * kHpglToPx)
                                      : std::max(4.0, motif);
        apDefs_[i] = std::move(def);
        apIndex_.insert(QByteArray(d.name), static_cast<int>(i));
    }

    atlas_ = pm;
    return true;
}

// ---- queries ----------------------------------------------------------------

uint16_t RenderResourceAtlas::findSymbol(const QByteArray& name) const {
    return nameIndex_.value(name, kNoSymbol);
}

// ---- HPGL compiler ----------------------------------------------------------

std::vector<RenderResourceAtlas::HpglStroke>
RenderResourceAtlas::compileHpgl(const QByteArray& hpgl) const {
    std::vector<HpglStroke> out;
    QPainterPath cur, poly;
    bool inPoly = false;
    double pw = 1.0, cx = 0, cy = 0;
    auto flush = [&]() { if (!cur.isEmpty()) { out.push_back({ cur, pw, false }); cur = QPainterPath(); } };

    for (QByteArray tok : hpgl.split(';')) {
        tok = tok.trimmed();
        if (tok.size() < 2) continue;
        const QByteArray cmd = tok.left(2);
        const QByteArray arg = tok.mid(2);

        if (cmd == "SP") { /* single-pen defs: colour comes from the def */ }
        else if (cmd == "SW") {
            bool ok = false; const double w = arg.toDouble(&ok);
            if (ok && w > 0 && w != pw) { flush(); pw = w; }
        }
        else if (cmd == "PU") {
            const auto n = parseNums(arg);
            for (std::size_t i = 0; i + 1 < n.size(); i += 2) { cx = n[i]; cy = n[i + 1]; }
            (inPoly ? poly : cur).moveTo(cx, cy);
        }
        else if (cmd == "PD") {
            const auto n = parseNums(arg);
            QPainterPath& t = inPoly ? poly : cur;
            if (t.isEmpty()) t.moveTo(cx, cy);
            for (std::size_t i = 0; i + 1 < n.size(); i += 2) { cx = n[i]; cy = n[i + 1]; t.lineTo(cx, cy); }
        }
        else if (cmd == "CI") {
            const double r = arg.toDouble();
            (inPoly ? poly : cur).addEllipse(QPointF(cx, cy), r, r);
        }
        else if (cmd == "PM") {
            const int m = arg.toInt();
            if (m == 0) { flush(); poly = QPainterPath(); poly.moveTo(cx, cy); inPoly = true; }
            else        { poly.closeSubpath(); inPoly = false; }
        }
        else if (cmd == "FP") { if (!poly.isEmpty()) out.push_back({ poly, pw, true  }); }
        else if (cmd == "EP") { if (!poly.isEmpty()) out.push_back({ poly, pw, false }); }
        // ST and others: ignored.
    }
    flush();
    return out;
}

// ---- drawing ----------------------------------------------------------------

void RenderResourceAtlas::draw(QPainter& p, uint16_t symIdx, QPointF d,
                               float rotationDeg, float scale) const {
    if (symIdx >= static_cast<uint16_t>(rects_.size())) return;
    const QRect&  src = rects_[symIdx];
    const QPoint& piv = pivots_[symIdx];

    if (rotationDeg == 0.0f) {
        if (scale == 1.0f) {
            p.drawPixmap(QPointF(d.x() - piv.x(), d.y() - piv.y()), atlas_, src);
        } else {
            QRectF dst(d.x() - piv.x() * scale, d.y() - piv.y() * scale,
                       src.width() * scale, src.height() * scale);
            p.drawPixmap(dst, atlas_, QRectF(src));
        }
        return;
    }
    const QTransform saved = p.transform();
    QTransform t = saved;
    t.translate(d.x(), d.y());
    t.rotate(rotationDeg);
    if (scale != 1.0f) t.scale(scale, scale);
    t.translate(-piv.x(), -piv.y());
    p.setTransform(t);
    p.drawPixmap(QPointF(0, 0), atlas_, src);
    p.setTransform(saved);
}

void RenderResourceAtlas::drawLineComplex(QPainter& p, int lcIndex,
                                          const QPolygonF& pts, float scale) const {
    if (lcIndex < 0 || lcIndex >= int(lcDefs_.size()) || pts.size() < 2) return;
    const LcDef& lc = lcDefs_[lcIndex];
    if (lc.strokes.empty()) return;

    const double s = kHpglToPx * scale;
    const double step  = std::max(2.0, lc.advance * s);   // motif repeat (px)
    const double reach = std::max(step, lc.reach * s);     // motif footprint (px)

    // Total on-screen length of the polyline. The motif is a fixed on-screen
    // size, so on a short feature the last stamp would otherwise paint past the
    // end ("too long, extending where it shouldn't" when zoomed out). Drop any
    // stamp whose footprint wouldn't fit, and skip lines shorter than one motif
    // entirely (the faint guide line in the vector pass still marks them).
    double total = 0.0;
    for (int i = 1; i < pts.size(); ++i)
        total += std::hypot(pts[i].x() - pts[i - 1].x(), pts[i].y() - pts[i - 1].y());
    if (total < reach) return;

    const QTransform saved = p.transform();
    p.resetTransform();

    QPen pen(lc.color); pen.setCosmetic(true);
    double next = 0.0;    // arc-length of the next stamp, measured from the start
    double base = 0.0;    // arc-length at the start of the current segment
    for (int i = 1; i < pts.size() && next + reach <= total + 0.5; ++i) {
        const QPointF a = pts[i - 1], b = pts[i];
        const double dx = b.x() - a.x(), dy = b.y() - a.y();
        const double len = std::hypot(dx, dy);
        if (len < 1e-6) continue;
        const double ang = std::atan2(dy, dx) * 180.0 / M_PI;
        const double ux = dx / len, uy = dy / len;

        while (next <= base + len && next + reach <= total + 0.5) {
            const double along = next - base;
            const QPointF pos(a.x() + ux * along, a.y() + uy * along);
            QTransform t;
            t.translate(pos.x(), pos.y());
            t.rotate(ang);
            t.scale(s, s);
            t.translate(-lc.pivot.x(), -lc.pivot.y());
            p.setTransform(t);
            for (const auto& stroke : lc.strokes) {
                if (stroke.fill) { p.setPen(Qt::NoPen); p.setBrush(lc.color); }
                else { pen.setWidthF(std::max(1.0, stroke.width * scale)); p.setPen(pen); p.setBrush(Qt::NoBrush); }
                p.drawPath(stroke.path);
            }
            next += step;
        }
        base += len;
    }
    p.setTransform(saved);
}

void RenderResourceAtlas::fillAreaPattern(QPainter& p, int apIndex,
                                          const QPainterPath& clip, QPointF anchor,
                                          float scale) const {
    if (apIndex < 0 || apIndex >= int(apDefs_.size())) return;
    const ApDef& ap = apDefs_[apIndex];
    if (ap.tile.isNull()) return;

    const double sp = std::max(2.0, ap.spacing * scale);
    const QSizeF ts(ap.tile.width() * scale, ap.tile.height() * scale);
    const QRectF b = clip.boundingRect();
    if (b.isEmpty()) return;

    p.save();
    p.setClipPath(clip, Qt::IntersectClip);   // honour any outer quilt clip
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Anchor the grid to a fixed scene point (mapped to `anchor`) so the pattern
    // stays put as the chart pans, and stagger alternate rows when required.
    const double oy = anchor.y() - std::floor((anchor.y() - b.top()) / sp + 1) * sp;
    int row = 0;
    for (double y = oy; y < b.bottom() + sp; y += sp, ++row) {
        const double rowShift = (ap.staggered && (row & 1)) ? sp * 0.5 : 0.0;
        const double ox = anchor.x() + rowShift
                          - std::floor((anchor.x() + rowShift - b.left()) / sp + 1) * sp;
        for (double x = ox; x < b.right() + sp; x += sp)
            p.drawImage(QRectF(QPointF(x, y), ts), ap.tile);
    }
    p.restore();
}
