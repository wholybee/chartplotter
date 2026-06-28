// tools/gen_symbols.cpp
//
// Build-time tool: parse chartsymbols.xml -> symbols.bin
//
// Usage: gen_symbols <chartsymbols.xml> <symbols.bin>
//
// This emits the S-52 look-up table (LUP) machinery plus every resource the
// runtime needs to *execute* an S-52 instruction at draw time:
//
//   - SY()  symbol stamps         (atlas tiles, baked from <symbols>/<bitmap>)
//   - LS()  simple line styles    (resolved at runtime via the colour table)
//   - AC()  area colour washes    (resolved at runtime via the colour table)
//   - LC()  complex lines         (HPGL vector defs, baked from <line-styles>)
//   - AP()  area patterns         (HPGL vector defs, baked from <patterns>)
//   - TX()/TE()  text labels      (parsed from the raw instruction at runtime)
//   - CS()  conditional symbology (executed in C++ at runtime; see sym_atlas)
//
// Unlike the previous generation (which pre-resolved a single SY/LS/AC per
// lookup), this build stores the *raw instruction string* for every lookup and
// lets the runtime parse and execute it.  That is what makes multi-symbol
// instructions, text, complex lines, area patterns, and conditional procedures
// possible.  The colour table (DAY_BRIGHT) and the HPGL line/pattern definitions
// are baked alongside so the runtime is fully self-contained from symbols.bin +
// the atlas PNG — chartsymbols.xml is still build-time only.
//
// Table selection (matches the chosen display style):
//   Point features -> "Paper" table   (realistic buoy/beacon shapes)
//   Area  features -> "Symbolized" table (symbolized boundaries + centred SY)
//   Line  features -> "Lines" table
// If a class has no lookup in the preferred table, the alternate point table
// ("Simplified") / area table ("Plain") is used as a fallback for that class.
//
// Binary layout (little-endian, packed):
//
//   Header (32 bytes)
//     char     magic[4]    = "SYM\x06"
//     uint32_t symCount    number of SymRecord
//     uint32_t lupCount    number of LupRecord
//     uint32_t condCount   number of CondRecord (attribute conditions pool)
//     uint32_t attrCount   number of AttrRecord (relevant-attribute acronyms)
//     uint32_t colorCount  number of ColorRecord (DAY_BRIGHT colour table)
//     uint32_t lcCount     number of LcDefRecord (LC line-complex HPGL defs)
//     uint32_t apCount     number of ApDefRecord (AP area-pattern HPGL defs)
//     uint32_t strBytes    size of the trailing string blob
//
//   SymRecord   x symCount   (36)  atlas tiles
//   LupRecord   x lupCount   (24)  one lookup (+ raw-instruction string ref)
//   CondRecord  x condCount  (32)  attribute condition pool
//   AttrRecord  x attrCount  (8)   relevant-attribute acronyms
//   ColorRecord x colorCount (12)  DAY_BRIGHT token -> RGBA
//   LcDefRecord x lcCount    (48)  line-complex HPGL definitions
//   ApDefRecord x apCount    (52)  area-pattern HPGL definitions
//   string blob x strBytes         instruction + HPGL text, referenced by offset
//
// All variable-length text (lookup instructions, HPGL programs) lives in the
// trailing string blob; records reference it by (offset,length).

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>
#include <QRegularExpression>
#include <QMap>
#include <QSet>
#include <QString>
#include <QByteArray>
#include <QVector>
#include <QList>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>

// ---- binary record types (packed) -------------------------------------------

#pragma pack(push, 1)
struct Header {
    char     magic[4];
    uint32_t symCount;
    uint32_t lupCount;
    uint32_t condCount;
    uint32_t attrCount;
    uint32_t colorCount;
    uint32_t lcCount;
    uint32_t apCount;
    uint32_t strBytes;
};
static_assert(sizeof(Header) == 36, "Header size");

struct SymRecord {
    char    name[24];
    int16_t atlas_x, atlas_y;
    int16_t width, height;
    int16_t pivot_x, pivot_y;
};
static_assert(sizeof(SymRecord) == 36, "SymRecord size");

struct LupRecord {
    char     objClass[8];
    uint8_t  geomType;      // 0=Point, 1=Line, 2=Area
    uint8_t  dispCat;       // 0=Displaybase, 1=Standard, 2=Other
    uint8_t  nConds;
    uint8_t  _pad;
    uint16_t condStart;
    uint16_t _pad2;
    uint32_t instrOff;      // offset of the raw instruction in the string blob
    uint16_t instrLen;
    uint16_t _pad3;
};
static_assert(sizeof(LupRecord) == 24, "LupRecord size");

struct CondRecord {
    char attr[8];
    char value[24];
};
static_assert(sizeof(CondRecord) == 32, "CondRecord size");

struct AttrRecord {
    char acronym[8];
};
static_assert(sizeof(AttrRecord) == 8, "AttrRecord size");

struct ColorRecord {
    char    token[8];       // null-padded 5-char S-52 colour token
    uint8_t r, g, b, a;
};
static_assert(sizeof(ColorRecord) == 12, "ColorRecord size");

struct LcDefRecord {        // LC() complex-line HPGL definition
    char     name[24];
    uint8_t  r, g, b, a;    // resolved pen colour
    uint16_t vecW, vecH;    // vector bbox (HPGL units)
    uint16_t pivotX, pivotY;
    uint16_t originX, originY;
    uint32_t hpglOff;
    uint16_t hpglLen;
    uint16_t _pad;
};
static_assert(sizeof(LcDefRecord) == 48, "LcDefRecord size");

struct ApDefRecord {        // AP() area-pattern definition (HPGL or raster)
    char     name[24];
    uint8_t  r, g, b, a;
    uint16_t vecW, vecH;
    uint16_t pivotX, pivotY;
    uint16_t originX, originY;
    uint16_t minDist, maxDist;
    uint8_t  fillType;      // 0=staggered(S), 1=linear(L)
    uint8_t  spacing;       // 0=constant(C), 1=scaled(S)
    uint8_t  hasBitmap;     // 1=use atlas tile (raster pattern), 0=HPGL
    uint8_t  _pad;
    int16_t  bmpX, bmpY;    // atlas tile origin (raster patterns)
    int16_t  bmpW, bmpH;    // atlas tile size  (raster patterns)
    uint32_t hpglOff;
    uint16_t hpglLen;
    uint16_t _pad2;
};
static_assert(sizeof(ApDefRecord) == 64, "ApDefRecord size");
#pragma pack(pop)

// ---- intermediate (parse-time) structures -----------------------------------

struct Cond { QByteArray attr; QByteArray value; };

struct Lup {
    QByteArray objClass;
    int        geomType = 0;   // 0 Point, 1 Line, 2 Area
    int        dispCat  = 1;   // 0 base, 1 standard, 2 other
    QByteArray table;          // "Paper", "Simplified", "Plain", "Symbolized", "Lines"
    QString    instruction;    // raw S-52 instruction (stored in string blob)
    QVector<Cond> conds;
};

struct VecDef {                // a baked HPGL line-style or pattern
    QByteArray name;
    QByteArray hpgl;
    uint8_t r = 0, g = 0, b = 0, a = 255;
    int vecW = 0, vecH = 0, pivotX = 0, pivotY = 0, originX = 0, originY = 0;
    int minDist = 0, maxDist = 0;
    uint8_t fillType = 0;      // patterns only
    uint8_t spacing  = 0;      // patterns only
    // Raster pattern (atlas tile) form: filled in when a pattern carries a
    // <bitmap>/<graphics-location> instead of (or alongside) HPGL.
    bool hasBitmap = false;
    int bmpX = 0, bmpY = 0, bmpW = 0, bmpH = 0;
};

// ---- helpers ----------------------------------------------------------------

static void padCopy(char* dst, int dstSize, const QByteArray& src) {
    std::memset(dst, 0, static_cast<std::size_t>(dstSize));
    int n = std::min(static_cast<int>(src.size()), dstSize - 1);
    std::memcpy(dst, src.constData(), static_cast<std::size_t>(n));
}

// Packed 0x00RRGGBB so we can use plain uint32_t in QtCore-only code.
static inline uint32_t packRgb(int r, int g, int b) {
    return (uint32_t(r & 0xFF) << 16) | (uint32_t(g & 0xFF) << 8) |
            uint32_t(b & 0xFF);
}

static int geomFromType(const QString& t) {
    if (t == QLatin1String("Point")) return 0;
    if (t == QLatin1String("Line"))  return 1;
    if (t == QLatin1String("Area"))  return 2;
    return 0;
}

static int catFromDisp(const QString& c) {
    if (c == QLatin1String("Displaybase")) return 0;
    if (c == QLatin1String("Other"))       return 2;
    return 1;   // Standard (default)
}

// Resolve a line-style / pattern <color-ref> to RGB.  Every line-style and
// pattern in chartsymbols.xml uses a single pen: a 6-char string of one pen-key
// char + a 5-char colour token (e.g. "ACHMGD" = key 'A' -> token "CHMGD").
static void resolveColorRef(const QByteArray& ref,
                            const QHash<QByteArray, uint32_t>& colors,
                            uint8_t& r, uint8_t& g, uint8_t& b) {
    r = g = b = 0;
    if (ref.size() < 6) return;
    const QByteArray tok = ref.mid(1, 5);
    const auto it = colors.constFind(tok);
    if (it == colors.constEnd()) return;
    const uint32_t rgb = it.value();
    r = uint8_t((rgb >> 16) & 0xFF);
    g = uint8_t((rgb >>  8) & 0xFF);
    b = uint8_t( rgb        & 0xFF);
}

// Attributes referenced by conditional-symbology (CS) procedures but not always
// present in a lookup condition.  The runtime CS engine reads these, so the
// loader must make them available on each feature.  (Acronyms only; values are
// read per feature.)  Kept in sync with the procedures in sym_atlas.cpp.
static const char* const kCsAttrs[] = {
    // LIGHTS05
    "COLOUR", "CATLIT", "LITCHR", "SIGGRP", "SIGPER", "SECTR1", "SECTR2",
    "VALNMR", "HEIGHT", "LITVIS", "EXCLIT", "ORIENT", "STATUS",
    // TOPMAR01 / DAYMAR
    "TOPSHP",
    // RESTRN01 / RESARE02
    "RESTRN", "CATREA",
    // OBSTRN04 / UWTROC / WRECKS02
    "VALSOU", "WATLEV", "CATOBS", "EXPSOU", "QUASOU", "NATSUR", "CATWRK",
    // DEPARE01/02 / DEPCNT02
    "DRVAL1", "DRVAL2", "VALDCO", "QUAPOS",
    // SLCONS03
    "CATSLC", "CONDTN",
    // Generic text source
    "OBJNAM",
};

// Pull every S-57 attribute acronym referenced by a TX()/TE() text command in
// an instruction, so the loader reads them for the label to render.
//   TX(ATTRIB,...)              -> "ATTRIB"
//   TE('format','A1,A2,...',..) -> "A1","A2"
static void collectTextAttrs(const QString& instr, QSet<QByteArray>& out) {
    static const QRegularExpression reTX(QStringLiteral(R"(TX\(\s*([A-Za-z0-9_]+))"));
    static const QRegularExpression reTE(QStringLiteral(R"(TE\(\s*'[^']*'\s*,\s*'([^']*)')"));
    auto it = reTX.globalMatch(instr);
    while (it.hasNext()) {
        const QByteArray a = it.next().captured(1).trimmed().toLatin1();
        if (a.size() >= 5) out.insert(a.left(6));
    }
    auto jt = reTE.globalMatch(instr);
    while (jt.hasNext()) {
        const QStringList attrs = jt.next().captured(1).split(',');
        for (const QString& s : attrs) {
            const QByteArray a = s.trimmed().toLatin1();
            if (a.size() >= 5) out.insert(a.left(6));
        }
    }
}

// ---- main -------------------------------------------------------------------

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    if (argc < 3) {
        std::fprintf(stderr, "Usage: gen_symbols <chartsymbols.xml> <symbols.bin>\n");
        return 1;
    }
    QFile xmlFile(QString::fromLocal8Bit(argv[1]));
    if (!xmlFile.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "Cannot open input: %s\n", argv[1]);
        return 1;
    }

    // ---- parse symbols + lookups + colours + line-styles + patterns ---------

    QVector<SymRecord> syms;
    QMap<QByteArray, int> symIndexByName;   // symbol name -> index in syms
    QVector<Lup> lups;
    QVector<VecDef> lineDefs;               // LC line-complex defs
    QVector<VecDef> patDefs;                // AP area-pattern defs

    // DAY_BRIGHT colour table (token -> 0x00RRGGBB).  Resolved eagerly; used to
    // bake LC/AP pen colours and emitted whole for runtime LS()/AC() resolution.
    QHash<QByteArray, uint32_t> colors;
    QVector<QByteArray> colorOrder;         // preserve file order for output

    QXmlStreamReader xml(&xmlFile);
    bool inSymbols = false, inLookups = false, inBitmap = false;
    bool inDayColorTable = false;
    bool inLineStyles = false, inPatterns = false;

    struct {
        QByteArray name; int16_t ax=0, ay=0, w=0, h=0, px=0, py=0;
        bool hasLoc = false;
    } sym;

    struct {
        QByteArray objClass; QString type, table, disp, instruction;
        QVector<Cond> conds;
    } look;

    VecDef vd;   // current line-style / pattern being parsed

    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.isStartElement()) {
            const auto tag = xml.name();
            if (tag == u"color-table") {
                inDayColorTable =
                    (xml.attributes().value(QStringLiteral("name")) == u"DAY_BRIGHT");
            }
            else if (inDayColorTable && tag == u"color") {
                const auto a = xml.attributes();
                const QByteArray name = a.value(QStringLiteral("name")).trimmed().toLatin1();
                const int r = a.value(QStringLiteral("r")).toInt();
                const int g = a.value(QStringLiteral("g")).toInt();
                const int b = a.value(QStringLiteral("b")).toInt();
                if (!colors.contains(name)) colorOrder.append(name);
                colors.insert(name, packRgb(r, g, b));
            }
            else if (tag == u"symbols")     inSymbols = true;
            else if (tag == u"lookups")     inLookups = true;
            else if (tag == u"line-styles") inLineStyles = true;
            else if (tag == u"patterns")    inPatterns = true;
            else if (inSymbols) {
                if      (tag == u"symbol") sym = {};
                else if (tag == u"name")   sym.name = xml.readElementText().trimmed().toLatin1();
                else if (tag == u"bitmap") {
                    sym.w = static_cast<int16_t>(xml.attributes().value(QStringLiteral("width")).toInt());
                    sym.h = static_cast<int16_t>(xml.attributes().value(QStringLiteral("height")).toInt());
                    inBitmap = true;
                }
                else if (inBitmap && tag == u"graphics-location") {
                    sym.ax = static_cast<int16_t>(xml.attributes().value(QStringLiteral("x")).toInt());
                    sym.ay = static_cast<int16_t>(xml.attributes().value(QStringLiteral("y")).toInt());
                    sym.hasLoc = true;
                }
                else if (inBitmap && tag == u"pivot") {
                    sym.px = static_cast<int16_t>(xml.attributes().value(QStringLiteral("x")).toInt());
                    sym.py = static_cast<int16_t>(xml.attributes().value(QStringLiteral("y")).toInt());
                }
            }
            else if (inLineStyles || inPatterns) {
                // line-style / pattern parsing.  HPGL and color-ref are siblings
                // of <vector>; pivot/origin/distance are children of <vector>
                // (and, for raster patterns, of <bitmap>).  inBitmap routes the
                // shared child tags to the raster tile rather than the vector.
                if      (tag == u"line-style" || tag == u"pattern") vd = {};
                else if (tag == u"name")      vd.name = xml.readElementText().trimmed().toLatin1();
                else if (tag == u"HPGL")      vd.hpgl = xml.readElementText().trimmed().toLatin1();
                else if (tag == u"color-ref") {
                    const QByteArray ref = xml.readElementText().trimmed().toLatin1();
                    resolveColorRef(ref, colors, vd.r, vd.g, vd.b);
                }
                else if (tag == u"filltype")  vd.fillType = (xml.readElementText().trimmed() == QLatin1String("L")) ? 1 : 0;
                else if (tag == u"spacing")   vd.spacing  = (xml.readElementText().trimmed() == QLatin1String("S")) ? 1 : 0;
                else if (tag == u"bitmap") {
                    vd.hasBitmap = true;
                    vd.bmpW = xml.attributes().value(QStringLiteral("width")).toInt();
                    vd.bmpH = xml.attributes().value(QStringLiteral("height")).toInt();
                    inBitmap = true;
                }
                else if (tag == u"graphics-location") {
                    vd.bmpX = xml.attributes().value(QStringLiteral("x")).toInt();
                    vd.bmpY = xml.attributes().value(QStringLiteral("y")).toInt();
                }
                else if (tag == u"vector") {
                    vd.vecW = xml.attributes().value(QStringLiteral("width")).toInt();
                    vd.vecH = xml.attributes().value(QStringLiteral("height")).toInt();
                }
                else if (tag == u"distance") {
                    vd.minDist = xml.attributes().value(QStringLiteral("min")).toInt();
                    vd.maxDist = xml.attributes().value(QStringLiteral("max")).toInt();
                }
                else if (tag == u"pivot" && !inBitmap) {
                    vd.pivotX = xml.attributes().value(QStringLiteral("x")).toInt();
                    vd.pivotY = xml.attributes().value(QStringLiteral("y")).toInt();
                }
                else if (tag == u"origin" && !inBitmap) {
                    vd.originX = xml.attributes().value(QStringLiteral("x")).toInt();
                    vd.originY = xml.attributes().value(QStringLiteral("y")).toInt();
                }
            }
            else if (inLookups) {
                if (tag == u"lookup") {
                    look = {};
                    look.objClass = xml.attributes().value(QStringLiteral("name")).trimmed().toLatin1();
                }
                else if (tag == u"type")        look.type  = xml.readElementText().trimmed();
                else if (tag == u"table-name")  look.table = xml.readElementText().trimmed();
                else if (tag == u"display-cat") look.disp  = xml.readElementText().trimmed();
                else if (tag == u"instruction") look.instruction = xml.readElementText().trimmed();
                else if (tag == u"attrib-code") {
                    // e.g. "BOYSHP4" or "COLOUR3,4,3".  When the entry is just
                    // the 6-char acronym (or acronym + whitespace), it is a
                    // *presence* marker — the lookup applies when the feature
                    // carries that attribute at all.  Encoded with value "*".
                    QByteArray raw = xml.readElementText().toLatin1();
                    raw.replace(' ', "");
                    if (raw.size() >= 6) {
                        Cond c;
                        c.attr  = raw.left(6);
                        c.value = raw.mid(6);
                        if (c.value.isEmpty()) c.value = "*";
                        look.conds.append(c);
                    }
                }
            }
        }
        else if (xml.isEndElement()) {
            const auto tag = xml.name();
            if      (tag == u"color-table") inDayColorTable = false;
            else if (tag == u"symbols")     inSymbols = false;
            else if (tag == u"lookups")     inLookups = false;
            else if (tag == u"line-styles") inLineStyles = false;
            else if (tag == u"patterns")    inPatterns = false;
            else if ((inSymbols || inLineStyles || inPatterns) && tag == u"bitmap") inBitmap = false;
            else if (inSymbols && tag == u"symbol") {
                if (!sym.name.isEmpty() && sym.hasLoc && sym.w > 0 && sym.h > 0) {
                    if (!symIndexByName.contains(sym.name)) {
                        SymRecord r{};
                        padCopy(r.name, sizeof(r.name), sym.name);
                        r.atlas_x = sym.ax; r.atlas_y = sym.ay;
                        r.width   = sym.w;  r.height  = sym.h;
                        r.pivot_x = sym.px; r.pivot_y = sym.py;
                        symIndexByName[sym.name] = syms.size();
                        syms.append(r);
                    }
                }
            }
            else if (inLineStyles && tag == u"line-style") {
                if (!vd.name.isEmpty() && !vd.hpgl.isEmpty()) lineDefs.append(vd);
            }
            else if (inPatterns && tag == u"pattern") {
                if (!vd.name.isEmpty() && (!vd.hpgl.isEmpty() || vd.hasBitmap))
                    patDefs.append(vd);
            }
            else if (inLookups && tag == u"lookup") {
                Lup l;
                l.objClass    = look.objClass;
                l.geomType    = geomFromType(look.type);
                l.dispCat     = catFromDisp(look.disp);
                l.table       = look.table.toLatin1();
                l.conds       = look.conds;
                l.instruction = look.instruction;
                if (!l.objClass.isEmpty() && !l.instruction.isEmpty())
                    lups.append(l);
            }
        }
    }

    if (xml.hasError()) {
        std::fprintf(stderr, "XML error at line %lld: %s\n",
                     (long long)xml.lineNumber(),
                     xml.errorString().toUtf8().constData());
        return 1;
    }

    // ---- select the preferred table per (class, geomType) -------------------
    //   Point: prefer "Paper", else "Simplified".
    //   Line:  "Lines" (the only line table).
    //   Area:  prefer "Symbolized", else "Plain".
    auto preferredTable = [](int geom, const QSet<QByteArray>& tablesPresent) -> QByteArray {
        if (geom == 0) {
            if (tablesPresent.contains("Paper"))      return "Paper";
            if (tablesPresent.contains("Simplified")) return "Simplified";
        } else if (geom == 1) {
            if (tablesPresent.contains("Lines"))      return "Lines";
        } else if (geom == 2) {
            if (tablesPresent.contains("Symbolized")) return "Symbolized";
            if (tablesPresent.contains("Plain"))      return "Plain";
        }
        return {};
    };

    QMap<QByteArray, QSet<QByteArray>> tablesByKey;
    auto keyOf = [](const QByteArray& cls, int geom) {
        return cls + "|" + QByteArray::number(geom);
    };
    for (const Lup& l : lups)
        tablesByKey[keyOf(l.objClass, l.geomType)].insert(l.table);

    // Keep every lookup in the preferred table (all carry a non-empty
    // instruction, executed at runtime).
    QVector<Lup> kept;
    for (const Lup& l : lups) {
        const QByteArray want =
            preferredTable(l.geomType, tablesByKey[keyOf(l.objClass, l.geomType)]);
        if (want.isEmpty() || l.table != want) continue;
        kept.append(l);
    }

    // Stable group by (class, geom) so each class's lookups are contiguous, and
    // within a group put more-specific (more conditions) first.
    std::stable_sort(kept.begin(), kept.end(), [](const Lup& a, const Lup& b) {
        if (a.objClass != b.objClass) return a.objClass < b.objClass;
        if (a.geomType != b.geomType) return a.geomType < b.geomType;
        return a.conds.size() > b.conds.size();
    });

    // ---- relevant-attribute set --------------------------------------------
    QSet<QByteArray> attrSet;
    for (const Lup& l : kept) {
        for (const Cond& c : l.conds) attrSet.insert(c.attr);
        collectTextAttrs(l.instruction, attrSet);   // TX()/TE() sources
        if (l.instruction.contains(QStringLiteral("ORIENT")))
            attrSet.insert(QByteArrayLiteral("ORIENT"));
    }
    for (const char* a : kCsAttrs) attrSet.insert(QByteArray(a));
    QList<QByteArray> attrList = attrSet.values();
    std::sort(attrList.begin(), attrList.end());

    // ---- assemble the string blob (instructions + HPGL) ---------------------
    QByteArray strBlob;
    auto internStr = [&](const QByteArray& s, uint32_t& off, uint16_t& len) {
        off = static_cast<uint32_t>(strBlob.size());
        len = static_cast<uint16_t>(std::min<int>(s.size(), 0xFFFF));
        strBlob.append(s.constData(), len);
    };

    // ---- write binary -------------------------------------------------------
    QFile out(QString::fromLocal8Bit(argv[2]));
    if (!out.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "Cannot write output: %s\n", argv[2]);
        return 1;
    }

    // Build LupRecords (filling condPool + string blob as we go).
    QVector<CondRecord> condPool;
    QVector<LupRecord> lupRecs;
    lupRecs.reserve(kept.size());
    for (const Lup& l : kept) {
        LupRecord lr{};
        padCopy(lr.objClass, sizeof(lr.objClass), l.objClass);
        lr.geomType  = static_cast<uint8_t>(l.geomType);
        lr.dispCat   = static_cast<uint8_t>(l.dispCat);
        lr.nConds    = static_cast<uint8_t>(std::min<int>(static_cast<int>(l.conds.size()), 255));
        lr.condStart = static_cast<uint16_t>(condPool.size());
        internStr(l.instruction.toLatin1(), lr.instrOff, lr.instrLen);
        for (int i = 0; i < lr.nConds; ++i) {
            CondRecord cr{};
            padCopy(cr.attr,  sizeof(cr.attr),  l.conds[i].attr);
            padCopy(cr.value, sizeof(cr.value), l.conds[i].value);
            condPool.append(cr);
        }
        lupRecs.append(lr);
    }

    // Build LC / AP definition records (HPGL into the string blob).
    QVector<LcDefRecord> lcRecs;
    for (const VecDef& d : lineDefs) {
        LcDefRecord r{};
        padCopy(r.name, sizeof(r.name), d.name);
        r.r = d.r; r.g = d.g; r.b = d.b; r.a = 255;
        r.vecW = uint16_t(d.vecW); r.vecH = uint16_t(d.vecH);
        r.pivotX = uint16_t(d.pivotX); r.pivotY = uint16_t(d.pivotY);
        r.originX = uint16_t(d.originX); r.originY = uint16_t(d.originY);
        internStr(d.hpgl, r.hpglOff, r.hpglLen);
        lcRecs.append(r);
    }
    QVector<ApDefRecord> apRecs;
    for (const VecDef& d : patDefs) {
        ApDefRecord r{};
        padCopy(r.name, sizeof(r.name), d.name);
        r.r = d.r; r.g = d.g; r.b = d.b; r.a = 255;
        r.vecW = uint16_t(d.vecW); r.vecH = uint16_t(d.vecH);
        r.pivotX = uint16_t(d.pivotX); r.pivotY = uint16_t(d.pivotY);
        r.originX = uint16_t(d.originX); r.originY = uint16_t(d.originY);
        r.minDist = uint16_t(d.minDist); r.maxDist = uint16_t(d.maxDist);
        r.fillType = d.fillType; r.spacing = d.spacing;
        r.hasBitmap = d.hasBitmap ? 1 : 0;
        r.bmpX = int16_t(d.bmpX); r.bmpY = int16_t(d.bmpY);
        r.bmpW = int16_t(d.bmpW); r.bmpH = int16_t(d.bmpH);
        if (!d.hpgl.isEmpty()) internStr(d.hpgl, r.hpglOff, r.hpglLen);
        apRecs.append(r);
    }

    Header hdr{};
    hdr.magic[0]='S'; hdr.magic[1]='Y'; hdr.magic[2]='M'; hdr.magic[3]='\x06';
    hdr.symCount   = static_cast<uint32_t>(syms.size());
    hdr.lupCount   = static_cast<uint32_t>(lupRecs.size());
    hdr.condCount  = static_cast<uint32_t>(condPool.size());
    hdr.attrCount  = static_cast<uint32_t>(attrList.size());
    hdr.colorCount = static_cast<uint32_t>(colorOrder.size());
    hdr.lcCount    = static_cast<uint32_t>(lcRecs.size());
    hdr.apCount    = static_cast<uint32_t>(apRecs.size());
    hdr.strBytes   = static_cast<uint32_t>(strBlob.size());

    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(syms.constData()),
              syms.size() * static_cast<qsizetype>(sizeof(SymRecord)));
    out.write(reinterpret_cast<const char*>(lupRecs.constData()),
              lupRecs.size() * static_cast<qsizetype>(sizeof(LupRecord)));
    out.write(reinterpret_cast<const char*>(condPool.constData()),
              condPool.size() * static_cast<qsizetype>(sizeof(CondRecord)));
    for (const QByteArray& a : attrList) {
        AttrRecord ar{};
        padCopy(ar.acronym, sizeof(ar.acronym), a);
        out.write(reinterpret_cast<const char*>(&ar), sizeof(ar));
    }
    for (const QByteArray& tok : colorOrder) {
        ColorRecord cr{};
        padCopy(cr.token, sizeof(cr.token), tok);
        const uint32_t rgb = colors.value(tok);
        cr.r = uint8_t((rgb >> 16) & 0xFF);
        cr.g = uint8_t((rgb >>  8) & 0xFF);
        cr.b = uint8_t( rgb        & 0xFF);
        cr.a = 255;
        out.write(reinterpret_cast<const char*>(&cr), sizeof(cr));
    }
    out.write(reinterpret_cast<const char*>(lcRecs.constData()),
              lcRecs.size() * static_cast<qsizetype>(sizeof(LcDefRecord)));
    out.write(reinterpret_cast<const char*>(apRecs.constData()),
              apRecs.size() * static_cast<qsizetype>(sizeof(ApDefRecord)));
    out.write(strBlob.constData(), strBlob.size());
    out.close();

    std::fprintf(stdout,
        "gen_symbols: %d syms, %d lookups, %d conds, %d attrs, "
        "%d colors, %d lines(LC), %d patterns(AP), %d str bytes\n",
        static_cast<int>(syms.size()), static_cast<int>(lupRecs.size()),
        static_cast<int>(condPool.size()), static_cast<int>(attrList.size()),
        static_cast<int>(colorOrder.size()), static_cast<int>(lcRecs.size()),
        static_cast<int>(apRecs.size()), static_cast<int>(strBlob.size()));
    std::fprintf(stdout, "gen_symbols: wrote %s (%lld bytes)\n",
                 argv[2], (long long)QFileInfo(out).size());
    return 0;
}
