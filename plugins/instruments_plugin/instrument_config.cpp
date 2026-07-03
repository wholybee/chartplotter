#include "instrument_config.hpp"
#include "nav_data_store.hpp"
#include "bundle_paths.hpp"

#include <QHash>
#include <QXmlStreamReader>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QDebug>

namespace {

// Map of path key (lower-case) -> pointer-to-member on OwnshipState. Keeping the
// table here means the settings docs and the runtime lookup never drift apart.
using Member = const NavValue OwnshipState::*;

const QHash<QString, Member>& pathTable() {
    static const QHash<QString, Member> t = {
        { QStringLiteral("latitudedeg"),            &OwnshipState::latitudeDeg },
        { QStringLiteral("longitudedeg"),           &OwnshipState::longitudeDeg },
        { QStringLiteral("cogdegtrue"),             &OwnshipState::cogDegTrue },
        { QStringLiteral("sogknots"),               &OwnshipState::sogKnots },
        { QStringLiteral("waterspeedknots"),        &OwnshipState::waterSpeedKnots },
        { QStringLiteral("headingdegtrue"),         &OwnshipState::headingDegTrue },
        { QStringLiteral("headingdegmag"),          &OwnshipState::headingDegMag },
        { QStringLiteral("variationdeg"),           &OwnshipState::variationDeg },
        { QStringLiteral("depthmeters"),            &OwnshipState::depthMeters },
        { QStringLiteral("apparentwindangledeg"),   &OwnshipState::apparentWindAngleDeg },
        { QStringLiteral("apparentwindspeedknots"), &OwnshipState::apparentWindSpeedKnots },
        { QStringLiteral("truewindangledeg"),       &OwnshipState::trueWindAngleDeg },
        { QStringLiteral("truewindspeedknots"),     &OwnshipState::trueWindSpeedKnots },
        { QStringLiteral("truewinddirectiondeg"),   &OwnshipState::trueWindDirectionDeg },
    };
    return t;
}

// Decode the display attribute into (analog?, gauge style). Anything not
// recognised as an analogue style ("analog"/"analogue", "compass", "wind") is a
// digital read-out.
void decodeDisplay(const QString& s, bool& analog, GaugeStyle& gauge) {
    const QString v = s.trimmed().toLower();
    if (v == QStringLiteral("compass")) { analog = true;  gauge = GaugeStyle::Compass; }
    else if (v == QStringLiteral("wind")) { analog = true; gauge = GaugeStyle::Wind; }
    else if (v.startsWith(QStringLiteral("analog"))) { analog = true; gauge = GaugeStyle::Arc; }
    else { analog = false; gauge = GaugeStyle::Arc; }
}

}  // namespace

const NavValue* navValueForPath(const OwnshipState& os, const QString& path) {
    const auto it = pathTable().constFind(path.trimmed().toLower());
    if (it == pathTable().constEnd()) return nullptr;
    return &(os.*(it.value()));
}

QStringList knownInstrumentPaths() {
    QStringList keys = pathTable().keys();
    keys.sort();
    return keys;
}

QList<InstrumentDef> InstrumentCatalog::parse(const QByteArray& xml) {
    QList<InstrumentDef> defs;
    QXmlStreamReader r(xml);
    while (!r.atEnd()) {
        if (r.readNext() != QXmlStreamReader::StartElement) continue;
        if (r.name() != QStringLiteral("instrument")) continue;

        const QXmlStreamAttributes a = r.attributes();
        InstrumentDef d;
        d.id      = a.value(QStringLiteral("id")).trimmed().toString();
        d.name    = a.value(QStringLiteral("name")).trimmed().toString();
        d.caption = a.value(QStringLiteral("caption")).trimmed().toString();
        d.path    = a.value(QStringLiteral("path")).trimmed().toString();
        d.unit    = a.value(QStringLiteral("unit")).toString();
        decodeDisplay(a.value(QStringLiteral("display")).toString(), d.analog, d.gauge);

        bool ok = false;
        const double factor = a.value(QStringLiteral("factor")).toDouble(&ok);
        d.factor = ok ? factor : 1.0;
        d.min = a.value(QStringLiteral("min")).toDouble();
        d.max = a.value(QStringLiteral("max")).toDouble();
        if (d.max <= d.min) d.max = d.min + 1.0;   // guard against a zero span
        const int dec = a.value(QStringLiteral("decimals")).toInt(&ok);
        d.decimals = (ok && dec >= 0) ? dec : 1;

        // An entry must at least carry an id and a path to be usable; fall back
        // to the path as a display name / caption when those are omitted.
        if (d.id.isEmpty() || d.path.isEmpty()) continue;
        if (d.name.isEmpty())    d.name = d.id;
        if (d.caption.isEmpty()) d.caption = d.name;
        defs.push_back(d);
    }
    if (r.hasError())
        qWarning() << "Instruments: XML parse error:" << r.errorString();
    return defs;
}

QString InstrumentCatalog::userConfigPath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath();
    return QDir(dir).filePath(QStringLiteral("instruments.xml"));
}

QList<InstrumentDef> InstrumentCatalog::load() {
    const QString userPath = userConfigPath();

    // 1) Prefer an existing user copy.
    if (QFileInfo::exists(userPath)) {
        QFile f(userPath);
        if (f.open(QIODevice::ReadOnly)) {
            QList<InstrumentDef> defs = parse(f.readAll());
            if (!defs.isEmpty()) {
                qInfo() << "Instruments: loaded catalogue from" << userPath;
                return defs;
            }
            qWarning() << "Instruments: user catalogue at" << userPath
                       << "had no usable entries; rebuilding from default.";
        }
    }

    // 2) Seed the user copy from the bundled default (next to the exe) when
    //    present, otherwise from the built-in fallback. Then load it back so the
    //    user always has an editable file at a known path.
    QByteArray seed;
    const QString bundled = QDir(bundlepaths::dataDir())
                                .filePath(QStringLiteral("instruments.xml"));
    if (QFileInfo::exists(bundled)) {
        QFile bf(bundled);
        if (bf.open(QIODevice::ReadOnly)) seed = bf.readAll();
    }
    if (seed.isEmpty()) seed = defaultXml();

    QDir().mkpath(QFileInfo(userPath).absolutePath());
    QFile out(userPath);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        out.write(seed);
        out.close();
        qInfo() << "Instruments: wrote default catalogue to" << userPath
                << "(edit this file to customise the available instruments).";
    } else {
        qWarning() << "Instruments: could not write catalogue to" << userPath
                   << "; using built-in defaults for this session.";
    }

    QList<InstrumentDef> defs = parse(seed);
    if (defs.isEmpty()) defs = parse(defaultXml());   // last-ditch safety net
    return defs;
}

QByteArray InstrumentCatalog::defaultXml() {
    // Kept identical to plugins/instruments_plugin/instruments.xml (the file
    // deployed next to the executable). This copy is the fallback used when that
    // file is missing, and the seed written to the user config on first run.
    return QByteArray(R"XML(<?xml version="1.0" encoding="UTF-8"?>
<!--
  Instrument catalogue for the Instruments plugin.

  Each <instrument> defines a tile that can be added to the instrument bar from
  Settings > Plugin Settings > Instruments.

  Attributes:
    id        stable identifier (referenced by the saved selection; keep unique)
    name      full name shown in the settings list
    caption   short label drawn on the tile (defaults to name)
    path      which nav-data value to show. One of:
                cogDegTrue, sogKnots, waterSpeedKnots, headingDegTrue,
                headingDegMag, variationDeg, depthMeters,
                apparentWindAngleDeg, apparentWindSpeedKnots,
                trueWindAngleDeg, trueWindSpeedKnots, trueWindDirectionDeg,
                latitudeDeg, longitudeDeg
    unit      unit label shown after the value (e.g. kn, m, deg)
    factor    multiply the stored value by this before display
                (stored units: speed=knots, depth=metres, angles=degrees)
    display   "digital"  numeric read-out
              "analog"   standard min..max arc gauge with a needle
              "compass"  north-up compass rose; needle points to the bearing
              "wind"     bow-up wind gauge: 0 at the top (the bow), starboard to
                         the right (positive), port to the left (negative)
    decimals  digital: number of decimal places (also used by the rose read-out)
    min, max  analog (arc) gauge range, in display units
              (ignored by the compass and wind gauges, which span a full circle)
-->
<instruments>
  <!-- Course over ground -->
  <instrument id="cog-digital" name="Course Over Ground (Digital)" caption="COG"
              path="cogDegTrue" unit="&#176;T" factor="1" display="digital" decimals="0"/>
  <instrument id="cog-analog" name="Course Over Ground (Analog)" caption="COG"
              path="cogDegTrue" unit="&#176;T" factor="1" display="analog" min="0" max="360"/>
  <instrument id="cog-compass" name="Course Over Ground (Compass)" caption="COG"
              path="cogDegTrue" unit="&#176;T" factor="1" display="compass"/>

  <!-- Speed over ground -->
  <instrument id="sog-digital" name="Speed Over Ground (Digital)" caption="SOG"
              path="sogKnots" unit="kn" factor="1" display="digital" decimals="1"/>
  <instrument id="sog-analog" name="Speed Over Ground (Analog)" caption="SOG"
              path="sogKnots" unit="kn" factor="1" display="analog" min="0" max="20"/>

  <!-- Speed through water -->
  <instrument id="stw-digital" name="Speed Through Water (Digital)" caption="STW"
              path="waterSpeedKnots" unit="kn" factor="1" display="digital" decimals="1"/>
  <instrument id="stw-analog" name="Speed Through Water (Analog)" caption="STW"
              path="waterSpeedKnots" unit="kn" factor="1" display="analog" min="0" max="20"/>

  <!-- Apparent wind speed -->
  <instrument id="aws-digital" name="Apparent Wind Speed (Digital)" caption="AWS"
              path="apparentWindSpeedKnots" unit="kn" factor="1" display="digital" decimals="1"/>
  <instrument id="aws-analog" name="Apparent Wind Speed (Analog)" caption="AWS"
              path="apparentWindSpeedKnots" unit="kn" factor="1" display="analog" min="0" max="40"/>

  <!-- Apparent wind direction (angle relative to the bow) -->
  <instrument id="awd-digital" name="Apparent Wind Direction (Digital)" caption="AWD"
              path="apparentWindAngleDeg" unit="&#176;" factor="1" display="digital" decimals="0"/>
  <instrument id="awd-analog" name="Apparent Wind Direction (Analog)" caption="AWD"
              path="apparentWindAngleDeg" unit="&#176;" factor="1" display="analog" min="0" max="360"/>
  <instrument id="awd-wind" name="Apparent Wind Direction (Wind Gauge)" caption="AWD"
              path="apparentWindAngleDeg" unit="&#176;" factor="1" display="wind"/>
</instruments>
)XML");
}
