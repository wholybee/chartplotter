#include "vessel_symbol.hpp"
#include <QPainter>
#include <QPolygonF>
#include <QPainterPath>
#include <QPen>

namespace vessel {

void drawSymbol(QPainter& p, const QPointF& pos,
                std::optional<double> headingDeg,
                double sogKnots, double predMinutes, double pixelsPerMetre,
                bool stale, const SymbolStyle& s, double scale) {
    p.save();
    p.resetTransform();                   // device pixels, constant size
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(pos);
    if (headingDeg) p.rotate(*headingDeg);
    if (scale != 1.0) p.scale(scale, scale);

    // Course-prediction line ahead (drawn first so the symbol sits on top): where
    // the vessel will be in predMinutes at the current SOG.
    if (sogKnots > 0.1 && pixelsPerMetre > 0.0 && predMinutes > 0.0) {
        const double dist_m = sogKnots * (1852.0 / 60.0) * predMinutes;
        // Prediction line length is a geographic distance — divide by scale so it
        // stays the same on-screen length regardless of the symbol size.
        const double len = (dist_m * pixelsPerMetre) / scale;
        if (len >= 1.0) {
            QPen line(s.predLine); line.setWidthF(1.5);
            p.setPen(line);
            p.setBrush(Qt::NoBrush);
            p.drawLine(QPointF(0, 0), QPointF(0, -len));
            // Small hollow circle at the head: diameter = half the glyph width
            // (glyph spans -8..+8, so width 16 -> radius 4).
            p.drawEllipse(QPointF(0, -len), 4.0, 4.0);
        }
    }

    const QColor strokeColor = stale ? s.staleEdge : s.edge;

    if (s.shape == SymbolStyle::Shape::BoatHull) {
        // Ownship: a simplified top-down boat hull — curved bow at the top, full
        // beam amidships, flat transom at the stern. Spans the same -14..+8
        // footprint as the AIS triangle so scale/highlight framing is unchanged.
        QPen edge(strokeColor); edge.setWidthF(1.2);
        edge.setJoinStyle(Qt::RoundJoin);
        p.setBrush(stale ? s.staleFill : s.fill);
        p.setPen(edge);
        if (headingDeg) {
            QPainterPath hull;
            hull.moveTo(0, -14);            // bow tip
            hull.quadTo(6.5, -8, 6, -1);    // starboard bow flare -> beam
            hull.lineTo(5, 6);              // starboard quarter
            hull.lineTo(4, 8);              // starboard transom corner
            hull.lineTo(-4, 8);             // port transom corner
            hull.lineTo(-5, 6);             // port quarter
            hull.lineTo(-6, -1);            // port beam
            hull.quadTo(-6.5, -8, 0, -14);  // port bow flare -> bow tip
            hull.closeSubpath();
            p.drawPath(hull);
        } else {
            p.drawEllipse(QPointF(0, 0), 7.0, 7.0);
        }
    } else if (s.shape == SymbolStyle::Shape::Chevron) {
        // Class B: filled arrowhead (a dart with a concave back notch).
        // Unknown heading falls back to a circle.
        QPen edge(strokeColor); edge.setWidthF(1.2);
        edge.setJoinStyle(Qt::RoundJoin);
        p.setBrush(stale ? s.staleFill : s.fill);
        p.setPen(edge);
        if (headingDeg) {
            // Tip at the bow; the two back wings match the Class A triangle's
            // footprint; a notch between them gives the arrowhead its dart shape.
            QPolygonF arrow;
            arrow << QPointF(0, -14)   // bow tip
                  << QPointF(8, 8)     // starboard wing
                  << QPointF(0, 2)     // back notch (concave)
                  << QPointF(-8, 8);   // port wing
            p.drawPolygon(arrow);
        } else {
            p.drawEllipse(QPointF(0, 0), 7.0, 7.0);
        }
    } else if (s.shape == SymbolStyle::Shape::Diamond) {
        // AtoN: an upright diamond with a centre dot. Orientation-independent
        // (the caller passes no heading); a virtual aid is drawn hollow because
        // its style carries a transparent fill.
        QPen edge(strokeColor); edge.setWidthF(1.4);
        edge.setJoinStyle(Qt::MiterJoin);
        p.setPen(edge);
        p.setBrush(stale ? s.staleFill : s.fill);
        QPolygonF diamond;
        diamond << QPointF(0, -11) << QPointF(8, 0) << QPointF(0, 11) << QPointF(-8, 0);
        p.drawPolygon(diamond);
        p.setBrush(strokeColor);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(0, 0), 2.0, 2.0);
    } else if (s.shape == SymbolStyle::Shape::Aircraft) {
        // SAR aircraft: a top-down plane silhouette pointing along heading/COG
        // (a circle when neither is known).
        QPen edge(strokeColor); edge.setWidthF(1.2);
        edge.setJoinStyle(Qt::RoundJoin);
        p.setBrush(stale ? s.staleFill : s.fill);
        p.setPen(edge);
        if (headingDeg) {
            QPainterPath plane;
            plane.moveTo(0, -13);          // nose
            plane.lineTo(1.6, -3);         // right fuselage shoulder
            plane.lineTo(11, 2);           // right wing tip (leading)
            plane.lineTo(11, 4);           // right wing tip (trailing)
            plane.lineTo(1.6, 3.5);        // back to fuselage
            plane.lineTo(1.6, 8);          // rear fuselage
            plane.lineTo(5, 11);           // right tailplane tip
            plane.lineTo(5, 12);
            plane.lineTo(0, 10);           // tail centre (notch)
            plane.lineTo(-5, 12);          // left tailplane
            plane.lineTo(-5, 11);
            plane.lineTo(-1.6, 8);
            plane.lineTo(-1.6, 3.5);
            plane.lineTo(-11, 4);          // left wing tip
            plane.lineTo(-11, 2);
            plane.lineTo(-1.6, -3);        // left fuselage shoulder
            plane.closeSubpath();
            p.drawPath(plane);
        } else {
            p.drawEllipse(QPointF(0, 0), 7.0, 7.0);
        }
    } else if (s.shape == SymbolStyle::Shape::SartCross) {
        // Distress beacon (AIS-SART / MOB / EPIRB): the IEC cross-in-circle, in
        // an alarming colour. Orientation-independent.
        QPen ring(strokeColor); ring.setWidthF(2.0);
        p.setPen(ring);
        p.setBrush(stale ? s.staleFill : s.fill);
        const double rad = 9.0;
        p.drawEllipse(QPointF(0, 0), rad, rad);
        p.drawLine(QPointF(0, -rad), QPointF(0, rad));     // cross: vertical
        p.drawLine(QPointF(-rad, 0), QPointF(rad, 0));     // cross: horizontal
    } else {
        // Class A / ownship: solid filled triangle (or circle when heading unknown).
        QPolygonF tri;
        tri << QPointF(0, -14) << QPointF(8, 8) << QPointF(-8, 8);
        QPen edge(strokeColor); edge.setWidthF(1.2);
        p.setBrush(stale ? s.staleFill : s.fill);
        p.setPen(edge);
        if (headingDeg) p.drawPolygon(tri);
        else            p.drawEllipse(QPointF(0, 0), 7.0, 7.0);
    }

    if (stale) {
        // Cancellation slash at right angles to the centerline (marine
        // convention for an unreliable / DR fix).
        QPen slash(strokeColor); slash.setWidthF(1.6);
        p.setPen(slash);
        p.drawLine(QPointF(-8.0, 0.0), QPointF(8.0, 0.0));
    }
    p.restore();
}

} // namespace vessel
