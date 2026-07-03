#include "chart_object_info_window.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QLabel>
#include <QHash>
#include <QStringList>
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>

#include "bundle_paths.hpp"

namespace {

// Shared palette / type face with the AIS target window, nav display window and
// instruments plugin (always dark, regardless of the OS theme).
const QString kText     = QStringLiteral("#e6e9ee");
const QString kCaption  = QStringLiteral("rgba(230,233,238,165)");
const QString kDim      = QStringLiteral("rgba(230,233,238,150)");
const QString kHairline = QStringLiteral("rgba(255,255,255,40)");

QFrame* makeSeparator(QWidget* parent) {
    auto* s = new QFrame(parent);
    s->setFrameShape(QFrame::HLine);
    s->setStyleSheet(QStringLiteral("color:%1;").arg(kHairline));
    return s;
}

// A compact S-57 object-class dictionary covering the classes a user is most
// likely to click. Unknown acronyms fall through to the acronym itself, so the
// window is still informative for the long tail.
const QHash<QString, QString>& classNames() {
    static const QHash<QString, QString> m = {
        {"BOYLAT", "Lateral buoy"},      {"BOYCAR", "Cardinal buoy"},
        {"BOYISD", "Isolated-danger buoy"}, {"BOYSAW", "Safe-water buoy"},
        {"BOYSPP", "Special-purpose buoy"}, {"BOYINB", "Installation buoy"},
        {"BCNLAT", "Lateral beacon"},    {"BCNCAR", "Cardinal beacon"},
        {"BCNISD", "Isolated-danger beacon"}, {"BCNSAW", "Safe-water beacon"},
        {"BCNSPP", "Special-purpose beacon"},
        {"LIGHTS", "Light"},             {"LITFLT", "Light float"},
        {"LITVES", "Light vessel"},      {"DAYMAR", "Daymark"},
        {"TOPMAR", "Topmark"},           {"RTPBCN", "Radar transponder beacon"},
        {"RADRFL", "Radar reflector"},   {"RETRFL", "Retroreflector"},
        {"FOGSIG", "Fog signal"},
        {"WRECKS", "Wreck"},             {"OBSTRN", "Obstruction"},
        {"UWTROC", "Underwater/awash rock"}, {"ROCKS",  "Rock"},
        {"SOUNDG", "Sounding"},          {"DEPCNT", "Depth contour"},
        {"DEPARE", "Depth area"},        {"DRGARE", "Dredged area"},
        {"COALNE", "Coastline"},         {"SLCONS", "Shoreline construction"},
        {"LNDARE", "Land area"},         {"LNDMRK", "Landmark"},
        {"LNDELV", "Land elevation"},    {"SLOTOP", "Slope topline"},
        {"PILPNT", "Pile / post"},       {"MORFAC", "Mooring facility"},
        {"ACHARE", "Anchorage area"},    {"ACHBRT", "Anchor berth"},
        {"RESARE", "Restricted area"},   {"CTNARE", "Caution area"},
        {"PRCARE", "Precautionary area"},{"TSSLPT", "Traffic-separation lane"},
        {"TSEZNE", "Traffic-separation zone"}, {"FAIRWY", "Fairway"},
        {"NAVLNE", "Navigation line"},   {"RECTRC", "Recommended track"},
        {"DWRTPT", "Deep-water route part"}, {"CBLSUB", "Submarine cable"},
        {"CBLOHD", "Overhead cable"},    {"PIPSOL", "Submarine pipeline"},
        {"BRIDGE", "Bridge"},            {"CAUSWY", "Causeway"},
        {"DAMCON", "Dam"},               {"LOCMAG", "Local magnetic anomaly"},
        {"SBDARE", "Seabed area"},       {"WEDKLP", "Weed / kelp"},
        {"SEAARE", "Sea area / named water"}, {"SPRING", "Spring"},
        {"BERTHS", "Berth"},             {"HRBFAC", "Harbour facility"},
        {"BUISGL", "Building (single)"}, {"BUAARE", "Built-up area"},
        {"PONTON", "Pontoon"},           {"FLODOC", "Floating dock"},
        {"HULKES", "Hulk"},              {"CRANES", "Crane"},
        {"MAGVAR", "Magnetic variation"},
    };
    return m;
}

// Friendly labels for the most common S-57 attribute acronyms. Values are left
// as their raw S-57 codes (decoding every enumerant is a larger undertaking);
// the label at least names each row.
QString attrLabel(const QString& acr) {
    static const QHash<QString, QString> m = {
        {"OBJNAM", "Name"},        {"COLOUR", "Colour"},     {"COLPAT", "Colour pattern"},
        {"BOYSHP", "Buoy shape"},  {"BCNSHP", "Beacon shape"},
        {"CATLAM", "Category (lateral)"}, {"CATCAM", "Category (cardinal)"},
        {"CATSPM", "Category (special)"}, {"CATLIT", "Category of light"},
        {"LITCHR", "Light character"},    {"SIGGRP", "Signal group"},
        {"SIGPER", "Signal period (s)"},  {"VALNMR", "Range (nm)"},
        {"HEIGHT", "Height (m)"},  {"VALSOU", "Sounding (m)"},
        {"DRVAL1", "Depth min (m)"}, {"DRVAL2", "Depth max (m)"},
        {"VERCLR", "Vertical clearance (m)"}, {"VERLEN", "Vertical length (m)"},
        {"WATLEV", "Water level"}, {"CATOBS", "Category of obstruction"},
        {"CATWRK", "Category of wreck"}, {"NATSUR", "Nature of surface"},
        {"INFORM", "Information"},  {"NOBJNM", "Name (national)"},
        {"ORIENT", "Orientation (°)"}, {"PEROND", "Period of day"},
        {"STATUS", "Status"},      {"CONVIS", "Conspicuous"},
    };
    auto it = m.constFind(acr);
    return it != m.constEnd() ? it.value() : acr;
}

// ---- S-57 enumerated-value decoding ---------------------------------------
//
// Enumerated ("E") and list ("L") S-57 attributes store their values as integer
// codes — COLOUR=4, BOYSHP=4, CATLAM=1 — whose human meaning lives in GDAL's
// catalogue CSVs, the very data folder the S-57 driver already needs:
//
//   s57attributes.csv     Code,Attribute,Acronym,Attributetype,Class
//                         → acronym → numeric attribute code (+ its type)
//   s57expectedinput.csv  Code,ID,Meaning
//                         → (attribute code, enumerant) → text
//
// Only E/L attributes are decoded; numeric (I/F) and free-text (A/S) values are
// real measurements / strings and pass through untouched. Missing files or
// unknown codes degrade gracefully to the raw number, like classNames() above.

// Split one CSV record into fields, honouring double-quoted fields that may
// themselves contain commas (e.g. "conical (nun, ogival)") or doubled quotes.
QStringList splitCsv(const QString& line) {
    QStringList out;
    QString cur;
    bool inQuote = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (inQuote) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                    cur += QLatin1Char('"'); ++i;          // escaped ""
                } else {
                    inQuote = false;
                }
            } else {
                cur += c;
            }
        } else if (c == QLatin1Char('"')) {
            inQuote = true;
        } else if (c == QLatin1Char(',')) {
            out << cur; cur.clear();
        } else {
            cur += c;
        }
    }
    out << cur;
    return out;
}

struct S57EnumTable {
    QHash<QString, int>    acronymCode;   // E/L acronyms → attribute code
    QHash<qint64, QString> meaning;       // (code,id) packed → text

    static qint64 key(int code, int id) {
        return (static_cast<qint64>(code) << 20) | static_cast<unsigned>(id);
    }

    // The bundled gdal-data folder (next to the exe, or Contents/Resources on
    // macOS — what main.cpp points GDAL at), falling back to a system GDAL_DATA
    // override.
    static QString dataDir() {
        const QString bundled = bundlepaths::dataDir()
                              + QStringLiteral("/gdal-data");
        if (QFile::exists(bundled + QStringLiteral("/s57attributes.csv")))
            return bundled;
        const QString env = qEnvironmentVariable("GDAL_DATA");
        if (!env.isEmpty()
            && QFile::exists(env + QStringLiteral("/s57attributes.csv")))
            return env;
        return QString();
    }

    void load() {
        const QString dir = dataDir();
        if (dir.isEmpty()) return;

        QFile attrs(dir + QStringLiteral("/s57attributes.csv"));
        if (attrs.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&attrs);
            in.readLine();                                  // header row
            while (!in.atEnd()) {
                const QStringList f = splitCsv(in.readLine());
                if (f.size() < 4) continue;
                const QString type = f.at(3).trimmed();
                if (type == QLatin1Char('E') || type == QLatin1Char('L')) {
                    bool ok = false;
                    const int code = f.at(0).trimmed().toInt(&ok);
                    if (ok) acronymCode.insert(f.at(2).trimmed(), code);
                }
            }
        }

        QFile expIn(dir + QStringLiteral("/s57expectedinput.csv"));
        if (expIn.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&expIn);
            in.readLine();                                  // header row
            while (!in.atEnd()) {
                const QStringList f = splitCsv(in.readLine());
                if (f.size() < 3) continue;
                bool okC = false, okI = false;
                const int code = f.at(0).trimmed().toInt(&okC);
                const int id   = f.at(1).trimmed().toInt(&okI);
                if (okC && okI) meaning.insert(key(code, id), f.at(2).trimmed());
            }
        }
    }
};

// Built once, lazily, on the GUI thread (info windows are only ever opened
// there). Empty maps when the CSVs are absent — decoding then becomes a no-op.
const S57EnumTable& s57EnumTable() {
    static const S57EnumTable t = [] { S57EnumTable x; x.load(); return x; }();
    return t;
}

// Decode an enumerated/list S-57 value to human text, e.g. COLOUR "1,4" →
// "white, green". Non-enumerated acronyms and unknown codes return the raw
// value unchanged.
QString decodeS57Value(const QString& acronym, const QString& raw) {
    const S57EnumTable& t = s57EnumTable();
    const auto it = t.acronymCode.constFind(acronym);
    if (it == t.acronymCode.constEnd()) return raw;        // not an enumerated attr
    const int code = it.value();

    QStringList parts;
    const QStringList toks = raw.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& tok : toks) {
        bool ok = false;
        const int id = tok.trimmed().toInt(&ok);
        const QString m = ok ? t.meaning.value(S57EnumTable::key(code, id)) : QString();
        parts << (m.isEmpty() ? tok.trimmed() : m);
    }
    return parts.isEmpty() ? raw : parts.join(QStringLiteral(", "));
}

} // namespace

QString chartObjectClassName(const QString& acronym) {
    auto it = classNames().constFind(acronym);
    return it != classNames().constEnd() ? it.value() : acronym;
}

ChartObjectInfoWindow::ChartObjectInfoWindow(const ChartObjectInfo& obj,
                                             DepthUnit depthUnit, QWidget* parent)
    : FramelessInfoDialog(parent) {
    const QString cls = chartObjectClassName(obj.objClass);
    const bool haveName = !obj.name.isEmpty();
    setWindowTitle(haveName ? QStringLiteral("%1 — %2").arg(cls, obj.name) : cls);

    // The frameless translucent window + rounded dark panel come from the base;
    // build our content into its layout.
    auto* col = panelLayout();

    // ---- Header: object identity, with the close button at the right. When the
    // object has a name that is the headline (class becomes the subtitle); the
    // raw S-57 acronym trails the subtitle for reference.
    auto* header = new QHBoxLayout;
    header->setSpacing(8);
    auto* idCol = new QVBoxLayout;
    idCol->setSpacing(1);

    auto* title = new QLabel(this);
    title->setStyleSheet(QStringLiteral("font-size:18px; font-weight:700; color:%1;").arg(kText));
    title->setWordWrap(true);
    auto* subtitle = new QLabel(this);
    subtitle->setStyleSheet(QStringLiteral("font-size:12px; color:%1;").arg(kDim));
    subtitle->setWordWrap(true);

    QStringList sub;
    if (haveName) {
        title->setText(obj.name);
        sub << cls;
        if (obj.objClass != cls) sub << obj.objClass;
    } else {
        title->setText(cls);
        if (obj.objClass != cls) sub << obj.objClass;
    }
    subtitle->setText(sub.join(QStringLiteral("  ·  ")));
    subtitle->setVisible(!sub.isEmpty());

    idCol->addWidget(title);
    idCol->addWidget(subtitle);
    header->addLayout(idCol, 1);
    header->addWidget(makeCloseButton(), 0, Qt::AlignTop);
    col->addLayout(header);

    col->addWidget(makeSeparator(panel()));

    // ---- Key facts (depth / position / scale).
    QVector<Detail> facts;
    if (obj.hasDepth) {
        const double v = (depthUnit == DepthUnit::Meters)
                           ? obj.depthM : obj.depthM * units::kMetersToFeet;
        const QString suffix = (depthUnit == DepthUnit::Meters) ? QStringLiteral(" m")
                                                                : QStringLiteral(" ft");
        facts.push_back({QStringLiteral("Depth"),
                         QString::number(v, 'f', v < 10.0 ? 1 : 0) + suffix, false});
    }
    facts.push_back({QStringLiteral("Lat"), units::formatLatitude(obj.lat),  false});
    facts.push_back({QStringLiteral("Lon"), units::formatLongitude(obj.lon), false});
    if (obj.scaleMin > 0)
        facts.push_back({QStringLiteral("Min scale"),
                         QStringLiteral("1:%1").arg(obj.scaleMin), false});

    auto* factsBox = new QWidget(this);
    auto* factsGrid = new QGridLayout(factsBox);
    factsGrid->setContentsMargins(0, 0, 0, 0);
    factsGrid->setHorizontalSpacing(10);
    factsGrid->setVerticalSpacing(5);
    fillGrid(factsGrid, factsBox, facts);
    col->addWidget(factsBox);

    // ---- Remaining captured S-57 attributes (skip OBJNAM — already the name).
    QVector<Detail> attrs;
    for (const ChartObjectAttr& a : obj.attrs) {
        if (a.key == QStringLiteral("OBJNAM") || a.value.isEmpty()) continue;
        const QString value = decodeS57Value(a.key, a.value);
        attrs.push_back({attrLabel(a.key), value, value.length() > 16});
    }
    if (!attrs.isEmpty()) {
        col->addWidget(makeSeparator(panel()));
        auto* hdr = new QLabel(QStringLiteral("Attributes"), this);
        hdr->setStyleSheet(QStringLiteral("font-size:12px; font-weight:600; color:%1;").arg(kCaption));
        col->addWidget(hdr);

        auto* attrsBox = new QWidget(this);
        auto* attrsGrid = new QGridLayout(attrsBox);
        attrsGrid->setContentsMargins(0, 0, 0, 0);
        attrsGrid->setHorizontalSpacing(10);
        attrsGrid->setVerticalSpacing(5);
        fillGrid(attrsGrid, attrsBox, attrs);
        col->addWidget(attrsBox);
    }

    col->addStretch(1);
}

void ChartObjectInfoWindow::fillGrid(QGridLayout* grid, QWidget* parent,
                                     const QVector<Detail>& entries) {
    auto makeCaption = [parent](const QString& text) {
        auto* l = new QLabel(text, parent);
        l->setStyleSheet(QStringLiteral("font-size:12px; color:%1;").arg(kDim));
        l->setMinimumWidth(58);
        l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        return l;
    };
    auto makeValue = [parent](const QString& text, bool wrap) {
        auto* l = new QLabel(text, parent);
        l->setStyleSheet(QStringLiteral("font-size:13px; font-weight:600; color:%1;").arg(kText));
        l->setWordWrap(wrap);
        l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        return l;
    };

    int row = 0, half = 0;   // half: 0 = left pair (cols 0/1), 1 = right pair (cols 2/3)
    for (const Detail& d : entries) {
        if (d.wide) {
            if (half == 1) { ++row; half = 0; }          // finish the open row first
            grid->addWidget(makeCaption(d.caption), row, 0);
            grid->addWidget(makeValue(d.value, true), row, 1, 1, 3);
            ++row; half = 0;
        } else {
            const int base = (half == 0) ? 0 : 2;
            grid->addWidget(makeCaption(d.caption), row, base);
            grid->addWidget(makeValue(d.value, false), row, base + 1);
            if (half == 0) half = 1;
            else { half = 0; ++row; }
        }
    }
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(3, 1);
}
