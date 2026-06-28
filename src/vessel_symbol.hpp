#pragma once
#include <QColor>
#include <QPointF>
#include <optional>

class QPainter;

// Shared vessel glyph renderer. Drawn in device pixels at constant on-screen
// size: a shape pointing along heading (a circle when heading is unknown),
// an optional course-prediction line ahead, dimmed with a cancellation slash
// when stale.
namespace vessel {

// Fore-aft length of the glyph in nominal pixels (bow tip at y=-14 to the stern
// base at y=+8). Exposed so callers that frame the glyph (e.g. a highlight ring)
// stay in step with the drawn geometry. Multiply by the symbol scale in use.
constexpr double kGlyphLengthPx = 22.0;

struct SymbolStyle {
    // FilledTriangle — Class A: solid filled triangle.
    // Chevron        — Class B: open arrowhead (two forward sides only, no
    //                  base, no fill). Matches the IALA/IHO Class B convention.
    // BoatHull       — ownship: a simplified top-down boat silhouette (curved
    //                  bow, full beam, transom stern) so it reads differently
    //                  from the AIS target wedges.
    // Diamond        — AtoN: an upright diamond with a centre dot (the standard
    //                  AIS aid-to-navigation symbol). Drawn hollow for a virtual
    //                  aid by passing a transparent fill. Ignores heading.
    // Aircraft       — SAR aircraft: a top-down plane silhouette along heading.
    // SartCross      — distress beacon (AIS-SART / MOB / EPIRB): a cross inside a
    //                  circle, the IEC distress symbol. Ignores heading.
    enum class Shape { FilledTriangle, Chevron, BoatHull, Diamond, Aircraft, SartCross };

    Shape  shape    = Shape::FilledTriangle;
    QColor fill;        // bright fill (FilledTriangle only)
    QColor staleFill;   // dimmed fill (FilledTriangle only)
    QColor edge;        // outline / chevron stroke colour (current)
    QColor staleEdge;   // outline / chevron stroke colour (stale)
    QColor predLine;    // course-prediction line
};

// `pos` is the device-pixel position. `headingDeg` absent => draw a circle and
// no rotation. The prediction line reaches where the vessel will be in
// `predMinutes` at `sogKnots`, using `pixelsPerMetre` to scale. `scale`
// multiplies all on-screen pixel sizes uniformly (1.0 = nominal).
void drawSymbol(QPainter& p, const QPointF& pos,
                std::optional<double> headingDeg,
                double sogKnots, double predMinutes, double pixelsPerMetre,
                bool stale, const SymbolStyle& style, double scale = 1.0);

} // namespace vessel
