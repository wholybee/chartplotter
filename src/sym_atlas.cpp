// src/sym_atlas.cpp
#include "sym_atlas.hpp"

#include <QFile>
#include <QPainter>
#include <QTransform>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// ---- binary format (must match tools/gen_symbols.cpp, magic "SYM\x06") -------

#pragma pack(push, 1)
struct BinHeader {
    char     magic[4];
    uint32_t symCount, lupCount, condCount, attrCount;
    uint32_t colorCount, lcCount, apCount, strBytes;
};
struct BinSymRecord {
    char    name[24];
    int16_t atlas_x, atlas_y, width, height, pivot_x, pivot_y;
};
struct BinLupRecord {
    char     objClass[8];
    uint8_t  geomType, dispCat, nConds, _pad;
    uint16_t condStart, _pad2;
    uint32_t instrOff;
    uint16_t instrLen, _pad3;
};
struct BinCondRecord { char attr[8]; char value[24]; };
struct BinAttrRecord { char acronym[8]; };
struct BinColorRecord { char token[8]; uint8_t r, g, b, a; };
struct BinLcDefRecord {
    char     name[24];
    uint8_t  r, g, b, a;
    uint16_t vecW, vecH, pivotX, pivotY, originX, originY;
    uint32_t hpglOff; uint16_t hpglLen, _pad;
};
struct BinApDefRecord {
    char     name[24];
    uint8_t  r, g, b, a;
    uint16_t vecW, vecH, pivotX, pivotY, originX, originY;
    uint16_t minDist, maxDist;
    uint8_t  fillType, spacing, hasBitmap, _pad;
    int16_t  bmpX, bmpY, bmpW, bmpH;
    uint32_t hpglOff; uint16_t hpglLen, _pad2;
};
#pragma pack(pop)

// These must match tools/gen_symbols.cpp exactly (binary format "SYM\x06").
static_assert(sizeof(BinHeader)      == 36, "BinHeader size");
static_assert(sizeof(BinSymRecord)   == 36, "BinSymRecord size");
static_assert(sizeof(BinLupRecord)   == 24, "BinLupRecord size");
static_assert(sizeof(BinCondRecord)  == 32, "BinCondRecord size");
static_assert(sizeof(BinAttrRecord)  == 8,  "BinAttrRecord size");
static_assert(sizeof(BinColorRecord) == 12, "BinColorRecord size");
static_assert(sizeof(BinLcDefRecord) == 48, "BinLcDefRecord size");
static_assert(sizeof(BinApDefRecord) == 64, "BinApDefRecord size");

// HPGL units -> pixels.  Calibrated so vector motifs render at roughly the same
// on-screen size as the prebaked point-symbol bitmaps (~0.034-0.038 px/unit).
static constexpr double kHpglToPx = 0.036;

// ---- SymHit helpers ---------------------------------------------------------

uint16_t SymHit::firstSym() const {
    return symbols.empty() ? SymAtlas::kNoSymbol : symbols.front().symIdx;
}
float SymHit::firstRot() const {
    return symbols.empty() ? 0.0f : symbols.front().rotationDeg;
}

// ---- key helper -------------------------------------------------------------

QByteArray SymAtlas::key(const QByteArray& objClass, SymGeom geom) {
    QByteArray k = objClass;
    k += '|';
    k += char('0' + static_cast<int>(geom));
    return k;
}

// ---- small parsing utilities ------------------------------------------------

namespace {

// Split `s` on `delim` at top level: not inside single quotes and not inside
// parentheses. Used to break an instruction into commands (';') and a command's
// argument list (',').
QList<QByteArray> splitTop(const QByteArray& s, char delim) {
    QList<QByteArray> out;
    int depth = 0; bool inq = false; QByteArray cur;
    for (char ch : s) {
        if (ch == '\'') { inq = !inq; cur += ch; }
        else if (!inq && ch == '(') { depth++; cur += ch; }
        else if (!inq && ch == ')') { depth--; cur += ch; }
        else if (!inq && depth == 0 && ch == delim) { out.append(cur); cur.clear(); }
        else cur += ch;
    }
    out.append(cur);
    return out;
}

QByteArray unquote(QByteArray s) {
    s = s.trimmed();
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'')
        return s.mid(1, s.size() - 2);
    return s;
}

// Parse a comma list of numbers (HPGL coordinate args).
std::vector<double> parseNums(const QByteArray& s) {
    std::vector<double> out;
    for (const QByteArray& p : s.split(',')) {
        const QByteArray t = p.trimmed();
        if (!t.isEmpty()) out.push_back(t.toDouble());
    }
    return out;
}

// Does a comma-joined S-57 multi-value (e.g. "1,4") contain `v`?
bool listHas(const std::string& csv, int v) {
    const QByteArray q = QByteArray::number(v);
    for (const QByteArray& p : QByteArray::fromStdString(csv).split(','))
        if (p.trimmed() == q) return true;
    return false;
}

} // namespace

// ---- load -------------------------------------------------------------------

bool SymAtlas::load(const QString& binPath, const QString& pngPath) {
    QPixmap pm(pngPath);
    if (pm.isNull()) return false;

    QFile f(binPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray raw = f.readAll();
    f.close();

    if (static_cast<std::size_t>(raw.size()) < sizeof(BinHeader)) return false;
    const auto* hdr = reinterpret_cast<const BinHeader*>(raw.constData());
    if (hdr->magic[0] != 'S' || hdr->magic[1] != 'Y' ||
        hdr->magic[2] != 'M' || hdr->magic[3] != '\x06')
        return false;

    const std::size_t symBytes   = std::size_t(hdr->symCount)   * sizeof(BinSymRecord);
    const std::size_t lupBytes   = std::size_t(hdr->lupCount)   * sizeof(BinLupRecord);
    const std::size_t condBytes  = std::size_t(hdr->condCount)  * sizeof(BinCondRecord);
    const std::size_t attrBytes  = std::size_t(hdr->attrCount)  * sizeof(BinAttrRecord);
    const std::size_t colorBytes = std::size_t(hdr->colorCount) * sizeof(BinColorRecord);
    const std::size_t lcBytes    = std::size_t(hdr->lcCount)    * sizeof(BinLcDefRecord);
    const std::size_t apBytes    = std::size_t(hdr->apCount)    * sizeof(BinApDefRecord);
    const std::size_t strBytes   = std::size_t(hdr->strBytes);
    if (static_cast<std::size_t>(raw.size()) <
            sizeof(BinHeader) + symBytes + lupBytes + condBytes + attrBytes +
            colorBytes + lcBytes + apBytes + strBytes)
        return false;

    const char* p = raw.constData() + sizeof(BinHeader);
    const auto* symRecs   = reinterpret_cast<const BinSymRecord*>(p);  p += symBytes;
    const auto* lupRecs   = reinterpret_cast<const BinLupRecord*>(p);  p += lupBytes;
    const auto* condRecs  = reinterpret_cast<const BinCondRecord*>(p); p += condBytes;
    const auto* attrRecs  = reinterpret_cast<const BinAttrRecord*>(p); p += attrBytes;
    const auto* colorRecs = reinterpret_cast<const BinColorRecord*>(p);p += colorBytes;
    const auto* lcRecs    = reinterpret_cast<const BinLcDefRecord*>(p);p += lcBytes;
    const auto* apRecs    = reinterpret_cast<const BinApDefRecord*>(p);p += apBytes;
    const char* strBase   = p;

    // Symbols.
    rects_.resize(hdr->symCount);
    pivots_.resize(hdr->symCount);
    nameIndex_.reserve(static_cast<int>(hdr->symCount));
    for (uint32_t i = 0; i < hdr->symCount; ++i) {
        const BinSymRecord& s = symRecs[i];
        rects_[i]  = QRect(s.atlas_x, s.atlas_y, s.width, s.height);
        pivots_[i] = QPoint(s.pivot_x, s.pivot_y);
        nameIndex_[QByteArray(s.name)] = static_cast<uint16_t>(i);
    }

    // Condition pool.
    conds_.resize(hdr->condCount);
    for (uint32_t i = 0; i < hdr->condCount; ++i) {
        conds_[i].attr  = std::string(condRecs[i].attr,
                                      strnlen(condRecs[i].attr, sizeof(condRecs[i].attr)));
        conds_[i].value = std::string(condRecs[i].value,
                                      strnlen(condRecs[i].value, sizeof(condRecs[i].value)));
    }

    // Lookups (grouped by class+geom; build the contiguous index).
    lups_.resize(hdr->lupCount);
    for (uint32_t i = 0; i < hdr->lupCount; ++i) {
        const BinLupRecord& l = lupRecs[i];
        lups_[i] = Lup{ l.geomType, l.dispCat, l.nConds, l.condStart,
                        l.instrOff, l.instrLen };
        const QByteArray k = key(QByteArray(l.objClass),
                                 static_cast<SymGeom>(l.geomType));
        auto it = lupIndex_.find(k);
        if (it == lupIndex_.end()) lupIndex_.insert(k, { i, 1u });
        else                       it.value().second += 1u;
    }

    // Relevant-attribute acronyms.
    attrs_.reserve(hdr->attrCount);
    for (uint32_t i = 0; i < hdr->attrCount; ++i)
        attrs_.emplace_back(attrRecs[i].acronym,
                            strnlen(attrRecs[i].acronym, sizeof(attrRecs[i].acronym)));

    // Colour table.
    for (uint32_t i = 0; i < hdr->colorCount; ++i) {
        const BinColorRecord& c = colorRecs[i];
        colorTable_.insert(QByteArray(c.token, strnlen(c.token, sizeof(c.token))),
                           QColor(c.r, c.g, c.b, c.a));
    }

    // String blob.
    strBlob_.assign(strBase, strBytes);

    // Compile LC line-complex definitions.
    lcDefs_.resize(hdr->lcCount);
    for (uint32_t i = 0; i < hdr->lcCount; ++i) {
        const BinLcDefRecord& d = lcRecs[i];
        LcDef def;
        def.color = QColor(d.r, d.g, d.b, d.a);
        def.advance = d.vecW > 0 ? double(d.vecW) : 1.0;
        def.pivot = QPointF(d.pivotX, d.pivotY);
        if (d.hpglLen && std::size_t(d.hpglOff) + d.hpglLen <= strBytes)
            def.strokes = compileHpgl(QByteArray(strBase + d.hpglOff, d.hpglLen));
        lcDefs_[i] = std::move(def);
        lcIndex_.insert(QByteArray(d.name), static_cast<int>(i));
    }

    // Build AP area-pattern tiles (raster copy or HPGL render).
    apDefs_.resize(hdr->apCount);
    for (uint32_t i = 0; i < hdr->apCount; ++i) {
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

    atlas_ = std::move(pm);
    return true;
}

// ---- queries ----------------------------------------------------------------

uint16_t SymAtlas::findSymbol(const QByteArray& name) const {
    return nameIndex_.value(name, kNoSymbol);
}

QColor SymAtlas::colorFor(const QByteArray& token) const {
    return colorTable_.value(token, QColor(0, 0, 0));
}

const std::string* SymAtlas::featVal(const AttrList& a, const char* acronym) const {
    for (const auto& kv : a)
        if (kv.first == acronym) return &kv.second;
    return nullptr;
}

SymHit SymAtlas::symbolForFeature(const QByteArray& objClass, SymGeom geom,
                                  const AttrList& attrs) const {
    SymHit hit;
    const auto it = lupIndex_.constFind(key(objClass, geom));
    if (it == lupIndex_.constEnd()) return hit;

    const uint32_t first = it.value().first;
    const uint32_t count = it.value().second;

    auto fv = [&attrs](const std::string& a) -> const std::string* {
        for (const auto& kv : attrs)
            if (kv.first == a) return &kv.second;
        return nullptr;
    };

    int        bestScore = -1;
    const Lup* bestLup   = nullptr;
    const Lup* defaultLup = nullptr;

    for (uint32_t i = first; i < first + count; ++i) {
        const Lup& l = lups_[i];
        if (l.nConds == 0) { if (!defaultLup) defaultLup = &l; continue; }
        bool matched = true;
        for (uint16_t c = 0; c < l.nConds; ++c) {
            const Cond& cond = conds_[l.condStart + c];
            const std::string* val = fv(cond.attr);
            if (!val) { matched = false; break; }
            if (cond.value != "*" && *val != cond.value) { matched = false; break; }
        }
        if (matched && static_cast<int>(l.nConds) > bestScore) {
            bestScore = l.nConds; bestLup = &l;
        }
    }
    const Lup* chosen = bestLup ? bestLup : defaultLup;
    if (!chosen) return hit;

    if (chosen->instrLen &&
        std::size_t(chosen->instrOff) + chosen->instrLen <= strBlob_.size()) {
        const QByteArray instr(strBlob_.data() + chosen->instrOff, chosen->instrLen);
        execInstruction(instr, objClass, geom, attrs, hit, 0);
    }
    return hit;
}

// ---- instruction executor ---------------------------------------------------

void SymAtlas::execInstruction(const QByteArray& instr, const QByteArray& objClass,
                               SymGeom geom, const AttrList& attrs, SymHit& hit,
                               int depth) const {
    if (depth > 4) return;

    for (QByteArray cmd : splitTop(instr, ';')) {
        cmd = cmd.trimmed();
        const int lp = cmd.indexOf('(');
        if (lp < 2) continue;
        const QByteArray verb = cmd.left(lp).trimmed();
        int rp = cmd.lastIndexOf(')');
        if (rp <= lp) rp = cmd.size();
        const QByteArray inner = cmd.mid(lp + 1, rp - lp - 1);
        const QList<QByteArray> args = splitTop(inner, ',');

        if (verb == "SY") {
            if (args.isEmpty()) continue;
            SymStamp st;
            st.symIdx = findSymbol(args[0].trimmed());
            if (st.symIdx == kNoSymbol) continue;
            if (args.size() > 1) {
                const QByteArray a = args[1].trimmed();
                if (a == "ORIENT") {
                    if (const std::string* o = featVal(attrs, "ORIENT"))
                        st.rotationDeg = float(std::atof(o->c_str()));
                } else {
                    bool ok = false; const float v = a.toFloat(&ok);
                    if (ok) st.rotationDeg = v;   // fixed-rotation literal
                }
            }
            hit.symbols.push_back(st);
        }
        else if (verb == "LS") {
            if (args.size() < 3 || hit.hasLine) continue;
            const QByteArray pat = args[0].trimmed();
            const QColor col = colorFor(args[2].trimmed());
            hit.line.pattern = (pat == "DASH") ? SymLineStyle::Dash
                             : (pat == "DOTT") ? SymLineStyle::Dot
                                               : SymLineStyle::Solid;
            hit.line.width = uint8_t(std::max(1, args[1].trimmed().toInt()));
            hit.line.r = col.red(); hit.line.g = col.green(); hit.line.b = col.blue();
            hit.hasLine = true;
        }
        else if (verb == "AC") {
            if (args.isEmpty() || hit.hasFill) continue;
            const QColor col = colorFor(args[0].trimmed());
            int tau = 0;
            if (args.size() > 1) tau = std::clamp(args[1].trimmed().toInt(), 0, 4);
            hit.fill.r = col.red(); hit.fill.g = col.green(); hit.fill.b = col.blue();
            hit.fill.a = uint8_t(255 - (tau * 255) / 4);
            hit.hasFill = true;
        }
        else if (verb == "LC") {
            if (args.isEmpty() || hit.lcIndex >= 0) continue;
            hit.lcIndex = lcIndex_.value(args[0].trimmed(), -1);
        }
        else if (verb == "AP") {
            if (args.isEmpty() || hit.apIndex >= 0) continue;
            hit.apIndex = apIndex_.value(args[0].trimmed(), -1);
        }
        else if (verb == "TX") {
            // TX(STRING,HJUST,VJUST,SPACE,CHARS,XOFFS,YOFFS,COLOUR,DISPLAY)
            if (args.isEmpty()) continue;
            SymText t;
            const QByteArray src = args[0].trimmed();
            QString text;
            if (src.startsWith('\'')) text = QString::fromUtf8(unquote(src));
            else if (const std::string* v = featVal(attrs, src.constData()))
                text = QString::fromUtf8(v->c_str());
            if (text.isEmpty()) continue;
            t.text = text;
            auto argi = [&](int i) { return (i < args.size()) ? args[i].trimmed() : QByteArray(); };
            t.hjust = uint8_t(std::clamp(argi(1).toInt(), 1, 3));
            t.vjust = uint8_t(std::clamp(argi(2).toInt(), 1, 3));
            t.space = uint8_t(std::max(1, argi(3).toInt()));
            const QByteArray chars = unquote(argi(4));
            t.pointSize = uint8_t(std::clamp(chars.size() > 3 ? chars.mid(3).toInt() - 1 : 8, 7, 11));
            t.xoffs = argi(5).toInt();
            t.yoffs = argi(6).toInt();
            const QColor col = colorFor(argi(7));
            t.r = col.red(); t.g = col.green(); t.b = col.blue();
            hit.texts.push_back(std::move(t));
        }
        else if (verb == "TE") {
            // TE('FORMAT','ATTRIBS',HJUST,VJUST,SPACE,CHARS,XOFFS,YOFFS,COLOUR,DISPLAY)
            if (args.size() < 2) continue;
            const QByteArray fmt = unquote(args[0]);
            const QList<QByteArray> aList = unquote(args[1]).split(',');
            // Gather attribute values; suppress the whole label if any is absent.
            QStringList vals; bool ok = true;
            for (const QByteArray& an : aList) {
                const std::string* v = featVal(attrs, an.trimmed().constData());
                if (!v || v->empty()) { ok = false; break; }
                vals << QString::fromUtf8(v->c_str());
            }
            if (!ok) continue;
            // Format: substitute each %-spec with the next attribute value.
            QString text; int vi = 0;
            for (int i = 0; i < fmt.size(); ++i) {
                const char ch = fmt[i];
                if (ch != '%') { text += QLatin1Char(ch); continue; }
                QByteArray spec("%"); ++i;
                while (i < fmt.size() &&
                       !QByteArray("diouxXeEfgGsc").contains(fmt[i])) {
                    if (fmt[i] != 'l' && fmt[i] != 'h') spec += fmt[i];
                    ++i;
                }
                const char conv = (i < fmt.size()) ? fmt[i] : 's';
                const QString val = (vi < vals.size()) ? vals[vi++] : QString();
                if (conv == 's') {
                    text += val;
                } else if (conv == 'd' || conv == 'i' || conv == 'u') {
                    text += QString::number(qlonglong(val.toDouble()));
                } else {                       // floating point
                    int prec = 1;
                    const int dot = spec.indexOf('.');
                    if (dot >= 0) prec = spec.mid(dot + 1).toInt();
                    text += QString::number(val.toDouble(), 'f', prec);
                }
            }
            if (text.trimmed().isEmpty()) continue;
            SymText t; t.text = text;
            auto argi = [&](int i) { return (i < args.size()) ? args[i].trimmed() : QByteArray(); };
            t.hjust = uint8_t(std::clamp(argi(2).toInt(), 1, 3));
            t.vjust = uint8_t(std::clamp(argi(3).toInt(), 1, 3));
            t.space = uint8_t(std::max(1, argi(4).toInt()));
            const QByteArray chars = unquote(argi(5));
            t.pointSize = uint8_t(std::clamp(chars.size() > 3 ? chars.mid(3).toInt() - 1 : 8, 7, 11));
            t.xoffs = argi(6).toInt();
            t.yoffs = argi(7).toInt();
            const QColor col = colorFor(argi(8));
            t.r = col.red(); t.g = col.green(); t.b = col.blue();
            hit.texts.push_back(std::move(t));
        }
        else if (verb == "CS") {
            if (!args.isEmpty())
                runCS(args[0].trimmed(), objClass, geom, attrs, hit, depth + 1);
        }
    }
}

// ---- conditional symbology procedures ---------------------------------------
//
// Each procedure synthesises a further instruction string (executed via
// execInstruction) and/or appends directly to the SymHit.  Ported from the
// S-52 presentation-library procedures (OpenCPN s52cnsy) to the extent the data
// available per feature allows.  Only the procedures that can actually reach the
// symbol engine for ENC chart features are implemented; depth-area, depth-
// contour, sounding, coastline and coverage procedures are handled by the
// chartplotter's native renderers instead.

namespace {

// Safety depth used to flag a danger (obstruction/wreck shallower than this gets
// the "dangerous" symbol).  A fixed default in metres; ECDIS would make this the
// user's safety contour.
constexpr double kSafetyDepthM = 20.0;

// TOPSHP (S-57 topmark/daymark shape) -> buoy/beacon topmark symbol name.
const char* topmarkSym(int topshp) {
    switch (topshp) {
        case 1:  return "TOPMAR02";   // cone, point up
        case 2:  return "TOPMAR04";   // cone, point down
        case 3:  return "TOPMAR10";   // sphere
        case 4:  return "TOPMAR12";   // 2 spheres
        case 5:  return "TOPMAR13";   // cylinder (can)
        case 6:  return "TOPMAR14";   // board
        case 7:  return "TOPMAR65";   // x-shape
        case 8:  return "TOPMAR86";   // upright cross
        case 9:  return "TOPMAR16";   // cube, point up
        case 10: return "TOPMAR08";   // 2 cones point to point  (West)
        case 11: return "TOPMAR07";   // 2 cones base to base    (East)
        case 12: return "TOPMAR99";   // rhombus
        case 13: return "TOPMAR05";   // 2 cones points up       (North)
        case 14: return "TOPMAR06";   // 2 cones points down     (South)
        case 15: return "TOPMAR88";   // besom, point up
        case 16: return "TOPMAR87";   // besom, point down
        case 17: return "TOPMAR17";   // flag
        case 18: return "TOPMAR10";   // sphere over rhombus -> sphere
        case 28: return "TOPMAR18";   // T-shape
        default: return "QUESMRK1";
    }
}

// S-57 LITCHR (light character) -> short abbreviation.
QString litchrAbbr(int v) {
    switch (v) {
        case 1:  return "F";     case 2:  return "Fl";   case 3:  return "LFl";
        case 4:  return "Q";     case 5:  return "VQ";   case 6:  return "UQ";
        case 7:  return "Iso";   case 8:  return "Oc";   case 9:  return "IQ";
        case 10: return "IVQ";   case 11: return "IUQ";  case 12: return "Mo";
        case 13: return "FFl";   case 14: return "FlLFl";case 16: return "OcFl";
        case 17: return "FLFl";  case 19: return "QLFl"; case 26: return "Al";
        default: return QString();
    }
}

// S-57 COLOUR code -> light-character colour letter.
QString colourLetter(int v) {
    switch (v) {
        case 1: return "W"; case 3: return "R"; case 4: return "G";
        case 6: return "Y"; case 11: return "Am"; case 13: return "Or";
        default: return QString();
    }
}

} // namespace

void SymAtlas::runCS(const QByteArray& proc, const QByteArray& objClass,
                     SymGeom geom, const AttrList& attrs, SymHit& hit,
                     int depth) const {
    auto val   = [&](const char* a) -> const std::string* { return featVal(attrs, a); };
    auto num   = [&](const char* a, double def) -> double {
        const std::string* v = featVal(attrs, a);
        return (v && !v->empty()) ? std::atof(v->c_str()) : def;
    };
    auto sy = [&](const char* name) {
        const uint16_t idx = findSymbol(QByteArray(name));
        if (idx != kNoSymbol) hit.symbols.push_back(SymStamp{ idx, 0.0f });
    };

    if (proc == "LIGHTS05") {
        const std::string* colour = val("COLOUR");
        const bool red   = colour && listHas(*colour, 3);
        const bool green = colour && listHas(*colour, 4);
        const bool sectored = val("SECTR1") && val("SECTR2");

        // Pick the coloured flare. Red/green get their own; everything else
        // (white, yellow, amber, …) uses the white/yellow flare.
        const char* flare = red ? "LIGHTS11" : green ? "LIGHTS12" : "LIGHTS13";
        SymStamp st; st.symIdx = findSymbol(QByteArray(flare));
        if (st.symIdx == kNoSymbol) st.symIdx = findSymbol(QByteArrayLiteral("LIGHTS14"));
        // Directional/sector lights orient the flare along SECTR1/ORIENT.
        if (const std::string* o = val("ORIENT"))
            st.rotationDeg = float(std::atof(o->c_str()));
        else if (const std::string* s1 = val("SECTR1"))
            st.rotationDeg = float(std::atof(s1->c_str()));
        if (st.symIdx != kNoSymbol) hit.symbols.push_back(st);

        // Build a compact light description ("Fl(2)R 10s15M") as a label.
        QString desc;
        if (const std::string* lc = val("LITCHR"))
            desc += litchrAbbr(std::atoi(lc->c_str()));
        if (const std::string* sg = val("SIGGRP")) {
            QString g = QString::fromUtf8(sg->c_str());
            if (!g.isEmpty() && g != QLatin1String("(1)")) {
                if (!g.startsWith('(')) g = '(' + g + ')';
                desc += g;
            }
        }
        if (colour)
            for (const QByteArray& c : QByteArray::fromStdString(*colour).split(','))
                desc += colourLetter(c.trimmed().toInt());
        if (const std::string* sp = val("SIGPER")) {
            const double per = std::atof(sp->c_str());
            if (per > 0) desc += QStringLiteral(" %1s").arg(per, 0, 'g', 3);
        }
        if (const double nm = num("VALNMR", -1); nm > 0)
            desc += QStringLiteral("%1M").arg(nm, 0, 'g', 2);
        if (!desc.trimmed().isEmpty() && !sectored) {
            SymText t; t.text = desc.trimmed();
            t.hjust = 3; t.vjust = 2; t.xoffs = 1; t.yoffs = 0;
            const QColor col = colorFor(QByteArrayLiteral("CHBLK"));
            t.r = col.red(); t.g = col.green(); t.b = col.blue();
            hit.texts.push_back(std::move(t));
        }
        return;
    }

    if (proc == "TOPMAR01" || proc == "TOPMARI1") {
        const int topshp = int(num("TOPSHP", 0));
        sy(topshp ? topmarkSym(topshp) : "QUESMRK1");
        return;
    }

    if (proc == "OBSTRN04") {
        const bool isRock = (objClass == "UWTROC");
        const double valsou = num("VALSOU", -1e9);
        const bool haveDepth = valsou > -1e8;
        const int watlev = int(num("WATLEV", 0));

        if (geom == SymGeom::Area) {
            // Area obstruction: fill+boundary handled by the area path; drop a
            // centred danger/obstruction glyph and a sounding label.
            sy(haveDepth && valsou <= kSafetyDepthM ? "ISODGR01" : "OBSTRN01");
        } else if (isRock) {
            if (watlev == 4 || watlev == 5) sy("UWTROC04");        // covers/uncovers/awash
            else if (haveDepth && valsou > kSafetyDepthM) { /* deep: sounding only */ }
            else sy("UWTROC03");                                   // dangerous rock
        } else {
            if (watlev == 1 || watlev == 2) sy("OBSTRN11");        // always above water
            else if (watlev == 4 || watlev == 5) sy("OBSTRN03");   // covers/uncovers
            else sy("OBSTRN01");                                   // submerged/unknown
        }

        if (haveDepth) {
            SymText t;
            t.text = (valsou == std::floor(valsou))
                         ? QString::number(qlonglong(valsou))
                         : QString::number(valsou, 'f', 1);
            t.hjust = 1; t.vjust = 2; t.yoffs = 1;
            const QColor col = colorFor(valsou <= kSafetyDepthM
                                            ? QByteArrayLiteral("CHBLK")
                                            : QByteArrayLiteral("CHGRD"));
            t.r = col.red(); t.g = col.green(); t.b = col.blue();
            hit.texts.push_back(std::move(t));
        }
        return;
    }

    if (proc == "WRECKS02") {
        const double valsou = num("VALSOU", -1e9);
        const bool haveDepth = valsou > -1e8;
        const int watlev = int(num("WATLEV", 0));
        const int catwrk = int(num("CATWRK", 0));

        if (haveDepth) {
            sy(valsou <= kSafetyDepthM ? "WRECKS05" : "WRECKS04");
            SymText t;
            t.text = (valsou == std::floor(valsou))
                         ? QString::number(qlonglong(valsou))
                         : QString::number(valsou, 'f', 1);
            t.hjust = 1; t.vjust = 2; t.yoffs = 1;
            const QColor col = colorFor(QByteArrayLiteral("CHBLK"));
            t.r = col.red(); t.g = col.green(); t.b = col.blue();
            hit.texts.push_back(std::move(t));
        } else if (watlev == 1 || watlev == 2 || watlev == 5) {
            sy("WRECKS01");                       // hull/superstructure showing
        } else if (catwrk == 1) {
            sy("WRECKS04");                       // non-dangerous
        } else if (catwrk == 2) {
            sy("WRECKS05");                       // dangerous
        } else {
            sy("WRECKS05");                       // unknown depth -> treat as danger
        }
        return;
    }

    if (proc == "RESTRN01") {
        // Restriction glyph for an area, picked from the RESTRN value list.
        const std::string* r = val("RESTRN");
        if (!r || r->empty()) return;
        const bool anchor  = listHas(*r, 1) || listHas(*r, 2);
        const bool fishing = listHas(*r, 3) || listHas(*r, 4) ||
                             listHas(*r, 5) || listHas(*r, 6);
        const bool entry   = listHas(*r, 7) || listHas(*r, 8);
        if (entry)        sy((anchor || fishing) ? "ENTRES71" : "ENTRES61");
        else if (anchor)  sy(fishing ? "ACHRES71" : "ACHRES61");
        else              sy("RSRDEF51");
        return;
    }

    if (proc == "RESARE02" || proc == "RESARE01") {
        // Restricted/anchorage area: dashed magenta boundary + centred glyph.
        if (!hit.hasLine) {
            const QColor col = colorFor(QByteArrayLiteral("CHMGF"));
            hit.line.pattern = SymLineStyle::Dash; hit.line.width = 2;
            hit.line.r = col.red(); hit.line.g = col.green(); hit.line.b = col.blue();
            hit.hasLine = true;
        }
        const std::string* r = val("RESTRN");
        if (r && !r->empty()) {
            runCS(QByteArrayLiteral("RESTRN01"), objClass, geom, attrs, hit, depth);
        } else if (objClass == "ACHARE") {
            sy("ACHARE02");
        } else {
            sy("ENTRES61");
        }
        return;
    }

    if (proc == "SYMINS01") {
        // New object: question-mark glyph (+ dashed boundary for areas).
        sy("QUESMRK1");
        if (geom == SymGeom::Area && !hit.hasLine) {
            const QColor col = colorFor(QByteArrayLiteral("CHMGD"));
            hit.line.pattern = SymLineStyle::Dash; hit.line.width = 1;
            hit.line.r = col.red(); hit.line.g = col.green(); hit.line.b = col.blue();
            hit.hasLine = true;
        }
        return;
    }

    // Unimplemented / not-applicable procedures (DEPARE/DEPCNT/SOUNDG/SLCONS/
    // QUAPOS/DATCVR/route/ownship): no-op. The feature still carries any direct
    // SY/LS/AC/text from the rest of its instruction.
}

// ---- HPGL compiler ----------------------------------------------------------

std::vector<SymAtlas::HpglStroke> SymAtlas::compileHpgl(const QByteArray& hpgl) const {
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

void SymAtlas::draw(QPainter& p, uint16_t symIdx, QPointF d,
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

void SymAtlas::drawLineComplex(QPainter& p, int lcIndex,
                               const QPolygonF& pts, float scale) const {
    if (lcIndex < 0 || lcIndex >= int(lcDefs_.size()) || pts.size() < 2) return;
    const LcDef& lc = lcDefs_[lcIndex];
    if (lc.strokes.empty()) return;

    const double s = kHpglToPx * scale;
    const double step = std::max(2.0, lc.advance * s);   // motif repeat (px)
    const QTransform saved = p.transform();
    p.resetTransform();

    QPen pen(lc.color); pen.setCosmetic(true);
    double carry = 0.0;   // distance into the current motif since the last stamp
    for (int i = 1; i < pts.size(); ++i) {
        const QPointF a = pts[i - 1], b = pts[i];
        const double dx = b.x() - a.x(), dy = b.y() - a.y();
        const double len = std::hypot(dx, dy);
        if (len < 1e-6) continue;
        const double ang = std::atan2(dy, dx) * 180.0 / M_PI;
        const double ux = dx / len, uy = dy / len;

        double along = step - carry;
        while (along <= len) {
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
            along += step;
        }
        carry = len - (along - step);
    }
    p.setTransform(saved);
}

void SymAtlas::fillAreaPattern(QPainter& p, int apIndex,
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
