#pragma once
// src/sym_atlas.hpp
//
// Runtime symbol atlas + S-52 look-up table (LUP) engine + instruction executor.
//
// Loads the prebaked symbols.bin (atlas tiles, per-class lookup tables with
// attribute conditions, the DAY_BRIGHT colour table, and the HPGL line-complex /
// area-pattern definitions) plus the rastersymbols-*.png sprite sheet.  Resolves
// a feature to a SymHit by:
//   1. best-match scoring the class's lookups against the feature's attributes,
//   2. executing the chosen lookup's S-52 instruction — including conditional
//      symbology procedures (CS), which expand into further instructions.
//
// The instruction language understood by the executor:
//   SY(sym[,ORIENT])  symbol stamp (optionally rotated by the ORIENT attribute)
//   LS(pat,wid,col)   simple line / area-boundary style
//   AC(col[,transp])  area colour wash
//   LC(linename)      complex line (HPGL motif stamped along the path)
//   AP(patname)       area pattern (HPGL/raster motif tiled inside the area)
//   TX(attr,...)      text label from a single attribute
//   TE('fmt',attr,..) formatted text label
//   CS(proc)          conditional symbology procedure (executed in C++)
//
// Thread safety: load() must run once on the GUI thread before any worker calls
// symbolForFeature().  After load() the data is immutable, so the query methods
// are safe to call concurrently from multiple worker threads.  The drawing
// helpers (draw / drawLineComplex / fillAreaPattern) run on the GUI thread.

#include <QPixmap>
#include <QImage>
#include <QRect>
#include <QRectF>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QPainterPath>
#include <QColor>
#include <QHash>
#include <QByteArray>
#include <QString>
#include <QPainter>
#include <vector>
#include <utility>
#include <string>
#include <cstdint>

// Geometry primitive of a feature, used to select the correct lookup table.
enum class SymGeom : uint8_t { Point = 0, Line = 1, Area = 2 };

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

// Result of resolving a feature: any combination of symbol stamps, a line
// style, a fill, a complex line (LC), an area pattern (AP), and text labels.
struct SymHit {
    std::vector<SymStamp> symbols;
    bool          hasLine = false;
    SymLineStyle  line;
    bool          hasFill = false;
    SymFillStyle  fill;
    int           lcIndex = -1;    // LC() line-complex def, or -1
    int           apIndex = -1;    // AP() area-pattern def, or -1
    std::vector<SymText>  texts;

    // Back-compat convenience: the first symbol's index/rotation, or kNoSymbol.
    uint16_t firstSym() const;
    float    firstRot() const;
};

class SymAtlas
{
public:
    static constexpr uint16_t kNoSymbol = 0xFFFFu;

    // A feature's symbology-relevant attributes: (6-char acronym, value string).
    using AttrList = std::vector<std::pair<std::string, std::string>>;

    bool load(const QString& binPath, const QString& pngPath);
    bool isLoaded() const { return !atlas_.isNull(); }

    // The S-57 attribute acronyms that any lookup condition, text command, or CS
    // procedure references. The chart loader reads exactly these per feature.
    const std::vector<std::string>& relevantAttrs() const { return attrs_; }

    // Resolve a feature (object class + geometry + attributes) to a SymHit via
    // S-52 best-match selection plus instruction execution (including CS).
    SymHit symbolForFeature(const QByteArray& objClass, SymGeom geom,
                            const AttrList& attrs) const;

    // Resolve a symbol name (e.g. "BOYPIL61") to its atlas index.
    uint16_t findSymbol(const QByteArray& name) const;

    // Draw symbol symIdx at screen point d, honouring the pivot offset.
    // rotationDeg rotates around the pivot (degrees CW from north). scale
    // multiplies the on-screen size around the pivot; 1.0 is the baked size.
    void draw(QPainter& p, uint16_t symIdx, QPointF d,
              float rotationDeg = 0.0f, float scale = 1.0f) const;

    // Stamp LC line-complex `lcIndex` repeatedly along a device-space polyline,
    // each motif rotated to the local tangent. scale matches symbol scaling.
    void drawLineComplex(QPainter& p, int lcIndex,
                         const QPolygonF& devicePts, float scale) const;

    // Fill `deviceClipPath` (device coords) with AP pattern `apIndex`, tiled and
    // anchored at `anchor` (the device position of a fixed scene point) so the
    // pattern stays put under panning. scale matches symbol scaling.
    void fillAreaPattern(QPainter& p, int apIndex,
                         const QPainterPath& deviceClipPath,
                         QPointF anchor, float scale) const;

    bool hasLineComplex(int i) const { return i >= 0 && i < int(lcDefs_.size()); }
    bool hasAreaPattern(int i) const { return i >= 0 && i < int(apDefs_.size()); }

private:
    struct Lup {
        uint8_t  geom;
        uint8_t  dispCat;
        uint8_t  nConds;
        uint16_t condStart;
        uint32_t instrOff;
        uint16_t instrLen;
    };
    struct Cond { std::string attr; std::string value; };

    // A compiled HPGL motif (line-style or pattern), in HPGL units.
    struct HpglStroke { QPainterPath path; double width = 1.0; bool fill = false; };
    struct LcDef {
        std::vector<HpglStroke> strokes;   // HPGL units
        QColor   color;
        double   advance = 0.0;            // repeat length (HPGL units)
        QPointF  pivot;                    // HPGL pivot (rides the line)
        double   reach = 0.0;              // motif extent right of pivot (HPGL units)
    };
    struct ApDef {
        QImage   tile;          // pre-rendered motif (RGBA, nominal scale)
        QPointF  tileOrigin;    // px offset of the motif's origin within tile
        double   spacing = 8.0; // px between motif anchors (nominal scale)
        bool     staggered = false;
    };

    // Instruction parsing/execution -----------------------------------------
    const std::string* featVal(const AttrList& a, const char* acronym) const;
    void execInstruction(const QByteArray& instr, const QByteArray& objClass,
                         SymGeom geom, const AttrList& attrs, SymHit& hit,
                         int depth) const;
    void runCS(const QByteArray& proc, const QByteArray& objClass, SymGeom geom,
               const AttrList& attrs, SymHit& hit, int depth) const;
    QColor colorFor(const QByteArray& token) const;

    // HPGL compilation (load time) ------------------------------------------
    std::vector<HpglStroke> compileHpgl(const QByteArray& hpgl) const;

    QPixmap atlas_;
    std::vector<QRect>  rects_;
    std::vector<QPoint> pivots_;
    QHash<QByteArray, uint16_t> nameIndex_;

    std::vector<Lup>          lups_;
    std::vector<Cond>         conds_;
    std::vector<std::string>  attrs_;
    std::string               strBlob_;                 // instruction + (unused) text pool
    QHash<QByteArray, QColor> colorTable_;              // S-52 token -> colour

    std::vector<LcDef>          lcDefs_;
    QHash<QByteArray, int>      lcIndex_;
    std::vector<ApDef>          apDefs_;
    QHash<QByteArray, int>      apIndex_;

    // (objClass|geom) -> contiguous [first,count) range into lups_.
    QHash<QByteArray, std::pair<uint32_t, uint32_t>> lupIndex_;

    static QByteArray key(const QByteArray& objClass, SymGeom geom);
};
