#pragma once
// src/portrayal_ir.hpp
//
// Stage 4: renderer-neutral presentation IR (renderer architecture plan,
// Layer 4 — docs/renderer_architecture_plan.md).
//
// The portrayal engine resolves a chart feature to a SymHit: a structured,
// renderer-neutral description of what to draw (symbol stamps, a line style, an
// area fill, a complex line, an area pattern, text labels, light sectors). It
// names no QPainter type and performs no drawing, so the same result can be
// consumed by the current QPainter build/paint path today and by the prepared
// GPU batch compiler in Stage 5.
//
// This is the contract between portrayal (portrayal_engine) and rendering
// (render_resource_atlas + chart_view build/paint). It corresponds to the
// architecture plan's RenderInstruction set, encoded here as one grouped result
// per feature rather than a flat instruction list.

#include <QString>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Geometry primitive of a feature, used to select the correct lookup table.
enum class SymGeom : uint8_t { Point = 0, Line = 1, Area = 2 };

// A feature's symbology-relevant attributes: (6-char acronym, value string).
using PortrayalAttrs = std::vector<std::pair<std::string, std::string>>;

// Sentinel for "no atlas entry" (symbol index not found / not applicable).
inline constexpr uint16_t kNoSymbol = 0xFFFFu;

// Simple line style from an LS() instruction. For a Line feature it styles the
// line; for an Area feature it styles the boundary outline.
struct SymLineStyle {
    enum Pattern : uint8_t { Solid = 0, Dash = 1, Dot = 2 };
    uint8_t pattern = Solid;
    uint8_t width   = 1;        // S-52 line-width units (~pixels)
    uint8_t r = 0, g = 0, b = 0;
};

// Translucent area-colour wash from an AC() instruction.
struct SymFillStyle {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

// One symbol stamp from a SY() instruction.
struct SymStamp {
    uint16_t symIdx      = 0xFFFFu;
    float    rotationDeg = 0.0f;   // S-57 ORIENT, degrees CW from true north
};

// One text label from a TX()/TE() instruction. Placement follows S-52:
//   hjust 1=centre 2=right 3=left   (horizontal alignment of the text box)
//   vjust 1=bottom 2=centre 3=top   (vertical alignment of the text box)
// xoffs/yoffs are in units of the nominal character box (≈ font width/height).
struct SymText {
    QString  text;
    uint8_t  hjust = 1, vjust = 1, space = 2;
    int      xoffs = 0, yoffs = 0;
    uint8_t  r = 0, g = 0, b = 0;
    uint8_t  pointSize = 8;        // resolved from the TX/TE body/style code
};

// One light sector arc from CS(LIGHTS05). A sectored S-57 LIGHTS feature carries
// one coloured sector. Its limits SECTR1/SECTR2 are bearings "from seaward"
// (observer→light); the chart arc is drawn around the light at the reciprocal
// (light→observer), so startDeg/endDeg are those limits + 180°, in degrees CW
// from true north, with the lit arc sweeping clockwise startDeg→endDeg.
struct SymSector {
    float   startDeg = 0.0f;   // SECTR1 + 180  (direction from light)
    float   endDeg   = 0.0f;   // SECTR1 + 180 + clockwise sweep
    float   rangeNm  = 0.0f;   // VALNMR nominal range, 0 = unknown
    uint8_t r = 0, g = 0, b = 0;   // resolved sector light colour
};

// Result of resolving a feature: any combination of symbol stamps, a line
// style, a fill, a complex line (LC), an area pattern (AP), text labels, and
// light-sector arcs.
struct SymHit {
    std::vector<SymStamp> symbols;
    bool          hasLine = false;
    SymLineStyle  line;
    bool          hasFill = false;
    SymFillStyle  fill;
    int           lcIndex = -1;    // LC() line-complex def, or -1
    int           apIndex = -1;    // AP() area-pattern def, or -1
    std::vector<SymText>  texts;
    std::vector<SymSector> sectors; // CS(LIGHTS05) light sectors

    // Back-compat convenience: the first symbol's index/rotation, or kNoSymbol.
    uint16_t firstSym() const {
        return symbols.empty() ? kNoSymbol : symbols.front().symIdx;
    }
    float firstRot() const {
        return symbols.empty() ? 0.0f : symbols.front().rotationDeg;
    }
};
