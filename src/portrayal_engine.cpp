// src/portrayal_engine.cpp
#include "portrayal_engine.hpp"
#include "render_resource_atlas.hpp"

#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

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

// Does a comma-joined S-57 multi-value (e.g. "1,4") contain `v`?
bool listHas(const std::string& csv, int v) {
    const QByteArray q = QByteArray::number(v);
    for (const QByteArray& p : QByteArray::fromStdString(csv).split(','))
        if (p.trimmed() == q) return true;
    return false;
}

} // namespace

// ---- PortrayalPackage -------------------------------------------------------

QByteArray PortrayalPackage::key(const QByteArray& objClass, SymGeom geom) {
    QByteArray k = objClass;
    k += '|';
    k += char('0' + static_cast<int>(geom));
    return k;
}

void PortrayalPackage::load(const BinLupRecord* lupRecs, uint32_t lupCount,
                            const BinCondRecord* condRecs, uint32_t condCount,
                            const BinAttrRecord* attrRecs, uint32_t attrCount,
                            const BinColorRecord* colorRecs, uint32_t colorCount,
                            const char* strBase, std::size_t strBytes) {
    // Condition pool.
    conds_.resize(condCount);
    for (uint32_t i = 0; i < condCount; ++i) {
        conds_[i].attr  = std::string(condRecs[i].attr,
                                      strnlen(condRecs[i].attr, sizeof(condRecs[i].attr)));
        conds_[i].value = std::string(condRecs[i].value,
                                      strnlen(condRecs[i].value, sizeof(condRecs[i].value)));
    }

    // Lookups (grouped by class+geom; build the contiguous index).
    lups_.resize(lupCount);
    for (uint32_t i = 0; i < lupCount; ++i) {
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
    attrs_.reserve(attrCount);
    for (uint32_t i = 0; i < attrCount; ++i)
        attrs_.emplace_back(attrRecs[i].acronym,
                            strnlen(attrRecs[i].acronym, sizeof(attrRecs[i].acronym)));

    // Colour table.
    for (uint32_t i = 0; i < colorCount; ++i) {
        const BinColorRecord& c = colorRecs[i];
        colorTable_.insert(QByteArray(c.token, strnlen(c.token, sizeof(c.token))),
                           QColor(c.r, c.g, c.b, c.a));
    }

    // String blob.
    strBlob_.assign(strBase, strBytes);
}

QColor PortrayalPackage::colorFor(const QByteArray& token) const {
    return colorTable_.value(token, QColor(0, 0, 0));
}

bool PortrayalPackage::lookupRange(const QByteArray& k, uint32_t& first,
                                   uint32_t& count) const {
    const auto it = lupIndex_.constFind(k);
    if (it == lupIndex_.constEnd()) return false;
    first = it.value().first;
    count = it.value().second;
    return true;
}

QByteArray PortrayalPackage::instruction(uint32_t off, uint16_t len) const {
    if (len == 0 || std::size_t(off) + len > strBlob_.size()) return QByteArray();
    return QByteArray(strBlob_.data() + off, len);
}

// ---- PortrayalEngine: evaluation --------------------------------------------

const std::string* PortrayalEngine::featVal(const PortrayalAttrs& a,
                                            const char* acronym) {
    for (const auto& kv : a)
        if (kv.first == acronym) return &kv.second;
    return nullptr;
}

SymHit PortrayalEngine::evaluate(const QByteArray& objClass, SymGeom geom,
                                 const PortrayalAttrs& attrs) const {
    SymHit hit;
    uint32_t first = 0, count = 0;
    if (!pkg_.lookupRange(PortrayalPackage::key(objClass, geom), first, count))
        return hit;

    auto fv = [&attrs](const std::string& a) -> const std::string* {
        for (const auto& kv : attrs)
            if (kv.first == a) return &kv.second;
        return nullptr;
    };

    int bestScore = -1;
    const PortrayalPackage::Lup* bestLup    = nullptr;
    const PortrayalPackage::Lup* defaultLup = nullptr;

    for (uint32_t i = first; i < first + count; ++i) {
        const PortrayalPackage::Lup& l = pkg_.lup(i);
        if (l.nConds == 0) { if (!defaultLup) defaultLup = &l; continue; }
        bool matched = true;
        for (uint16_t c = 0; c < l.nConds; ++c) {
            const PortrayalPackage::Cond& cond = pkg_.cond(l.condStart + c);
            const std::string* val = fv(cond.attr);
            if (!val) { matched = false; break; }
            if (cond.value != "*" && *val != cond.value) { matched = false; break; }
        }
        if (matched && static_cast<int>(l.nConds) > bestScore) {
            bestScore = l.nConds; bestLup = &l;
        }
    }
    const PortrayalPackage::Lup* chosen = bestLup ? bestLup : defaultLup;
    if (!chosen) return hit;

    const QByteArray instr = pkg_.instruction(chosen->instrOff, chosen->instrLen);
    if (!instr.isEmpty())
        execInstruction(instr, objClass, geom, attrs, hit, 0);
    return hit;
}

// ---- instruction executor ---------------------------------------------------

void PortrayalEngine::execInstruction(const QByteArray& instr,
                                      const QByteArray& objClass, SymGeom geom,
                                      const PortrayalAttrs& attrs, SymHit& hit,
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
            st.symIdx = res_.findSymbol(args[0].trimmed());
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
            const QColor col = pkg_.colorFor(args[2].trimmed());
            hit.line.pattern = (pat == "DASH") ? SymLineStyle::Dash
                             : (pat == "DOTT") ? SymLineStyle::Dot
                                               : SymLineStyle::Solid;
            hit.line.width = uint8_t(std::max(1, args[1].trimmed().toInt()));
            hit.line.r = col.red(); hit.line.g = col.green(); hit.line.b = col.blue();
            hit.hasLine = true;
        }
        else if (verb == "AC") {
            if (args.isEmpty() || hit.hasFill) continue;
            const QColor col = pkg_.colorFor(args[0].trimmed());
            int tau = 0;
            if (args.size() > 1) tau = std::clamp(args[1].trimmed().toInt(), 0, 4);
            hit.fill.r = col.red(); hit.fill.g = col.green(); hit.fill.b = col.blue();
            hit.fill.a = uint8_t(255 - (tau * 255) / 4);
            hit.hasFill = true;
        }
        else if (verb == "LC") {
            if (args.isEmpty() || hit.lcIndex >= 0) continue;
            hit.lcIndex = res_.lineComplexIndex(args[0].trimmed());
        }
        else if (verb == "AP") {
            if (args.isEmpty() || hit.apIndex >= 0) continue;
            hit.apIndex = res_.areaPatternIndex(args[0].trimmed());
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
            const QColor col = pkg_.colorFor(argi(7));
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
            const QColor col = pkg_.colorFor(argi(8));
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

void PortrayalEngine::runCS(const QByteArray& proc, const QByteArray& objClass,
                            SymGeom geom, const PortrayalAttrs& attrs, SymHit& hit,
                            int depth) const {
    auto val   = [&](const char* a) -> const std::string* { return featVal(attrs, a); };
    auto num   = [&](const char* a, double def) -> double {
        const std::string* v = featVal(attrs, a);
        return (v && !v->empty()) ? std::atof(v->c_str()) : def;
    };
    auto sy = [&](const char* name) {
        const uint16_t idx = res_.findSymbol(QByteArray(name));
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
        SymStamp st; st.symIdx = res_.findSymbol(QByteArray(flare));
        if (st.symIdx == kNoSymbol) st.symIdx = res_.findSymbol(QByteArrayLiteral("LIGHTS14"));
        // Directional/sector lights orient the flare along SECTR1/ORIENT.
        if (const std::string* o = val("ORIENT"))
            st.rotationDeg = float(std::atof(o->c_str()));
        else if (const std::string* s1 = val("SECTR1"))
            st.rotationDeg = float(std::atof(s1->c_str()));
        if (st.symIdx != kNoSymbol) hit.symbols.push_back(st);

        // Sector arc. A LIGHTS feature with both limits defines one coloured
        // sector. The limits are bearings from seaward (observer→light); the arc
        // is drawn around the light at the reciprocal (light→observer), sweeping
        // clockwise SECTR1→SECTR2. All-round "sectors" (limits equal / full
        // circle) carry no arc — they're just an ordinary light.
        if (sectored) {
            const double s1 = std::atof(val("SECTR1")->c_str());
            const double s2 = std::atof(val("SECTR2")->c_str());
            double sweep = std::fmod(s2 - s1, 360.0);
            if (sweep < 0.0) sweep += 360.0;
            if (sweep > 0.5 && sweep < 359.5) {
                SymSector sec;
                sec.startDeg = float(s1 + 180.0);
                sec.endDeg   = float(s1 + 180.0 + sweep);
                sec.rangeNm  = float(num("VALNMR", 0.0));
                const QColor c = pkg_.colorFor(red   ? QByteArrayLiteral("LITRD")
                                              : green ? QByteArrayLiteral("LITGN")
                                                      : QByteArrayLiteral("LITYW"));
                sec.r = uint8_t(c.red());
                sec.g = uint8_t(c.green());
                sec.b = uint8_t(c.blue());
                hit.sectors.push_back(sec);
            }
        }

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
            const QColor col = pkg_.colorFor(QByteArrayLiteral("CHBLK"));
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
            const QColor col = pkg_.colorFor(valsou <= kSafetyDepthM
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
            const QColor col = pkg_.colorFor(QByteArrayLiteral("CHBLK"));
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
            const QColor col = pkg_.colorFor(QByteArrayLiteral("CHMGF"));
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
            const QColor col = pkg_.colorFor(QByteArrayLiteral("CHMGD"));
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
