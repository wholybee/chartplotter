#pragma once
#include <QString>
#include <QChar>
#include <QList>
#include <cmath>

// Shared unit preferences. Kept in a tiny header (no Qt object, no .cpp) so the
// core Settings store, the Units dialog, and the chart view can all agree on the
// same enums without depending on one another. Persisted as short stable keys
// ("feet", "nm", …) rather than enum ordinals so the on-disk values survive
// reordering of the enums.

enum class DepthUnit { Feet, Meters };
enum class DistanceUnit { NauticalMiles, StatuteMiles, Kilometers };
// How a latitude/longitude is displayed (and accepted as input).
enum class AngleFormat { DecimalDegrees, DegMinutes, DegMinSec };
// Whether bearings and headings are shown relative to true or magnetic north.
enum class BearingMode { True, Magnetic };

namespace units {

constexpr double kMetersToFeet = 3.280839895;

// ---- depth ----------------------------------------------------------------

inline QString depthUnitKey(DepthUnit u) {
    return u == DepthUnit::Meters ? QStringLiteral("meters") : QStringLiteral("feet");
}
inline DepthUnit depthUnitFromKey(const QString& s, DepthUnit fallback = DepthUnit::Feet) {
    if (s == QStringLiteral("meters")) return DepthUnit::Meters;
    if (s == QStringLiteral("feet"))   return DepthUnit::Feet;
    return fallback;
}
inline QString depthUnitLabel(DepthUnit u) {
    return u == DepthUnit::Meters ? QStringLiteral("Meters (m)")
                                  : QStringLiteral("Feet (ft)");
}

// ---- distance --------------------------------------------------------------

inline QString distanceUnitKey(DistanceUnit u) {
    switch (u) {
        case DistanceUnit::StatuteMiles: return QStringLiteral("mi");
        case DistanceUnit::Kilometers:   return QStringLiteral("km");
        case DistanceUnit::NauticalMiles: break;
    }
    return QStringLiteral("nm");
}
inline DistanceUnit distanceUnitFromKey(const QString& s,
                                        DistanceUnit fallback = DistanceUnit::NauticalMiles) {
    if (s == QStringLiteral("mi")) return DistanceUnit::StatuteMiles;
    if (s == QStringLiteral("km")) return DistanceUnit::Kilometers;
    if (s == QStringLiteral("nm")) return DistanceUnit::NauticalMiles;
    return fallback;
}
inline QString distanceUnitLabel(DistanceUnit u) {
    switch (u) {
        case DistanceUnit::StatuteMiles: return QStringLiteral("Statute miles (mi)");
        case DistanceUnit::Kilometers:   return QStringLiteral("Kilometers (km)");
        case DistanceUnit::NauticalMiles: break;
    }
    return QStringLiteral("Nautical miles (nm)");
}

// Conversion factors from nautical miles. The metres-per-unit these imply match
// the scale bar in ChartView (1 nm = 1852 m, 1 statute mile = 1609.344 m,
// 1 km = 1000 m), so on-chart distances stay consistent across the app.
constexpr double kNmToStatuteMiles = 1852.0 / 1609.344;   // ~1.150779
constexpr double kNmToKilometers   = 1.852;

inline double distanceFromNm(double nm, DistanceUnit u) {
    switch (u) {
        case DistanceUnit::StatuteMiles: return nm * kNmToStatuteMiles;
        case DistanceUnit::Kilometers:   return nm * kNmToKilometers;
        case DistanceUnit::NauticalMiles: break;
    }
    return nm;
}

// Format a distance (given in nautical miles) in the user's selected unit, with a
// short suffix ("nm"/"mi"/"km"). Precision adapts to magnitude so short legs keep
// useful resolution without over-cluttering long ones.
inline QString formatDistance(double nm, DistanceUnit u) {
    const double v = distanceFromNm(nm, u);
    const int prec = v < 10.0 ? 2 : (v < 100.0 ? 1 : 0);
    return QString::number(v, 'f', prec) + QLatin1Char(' ') + distanceUnitKey(u);
}

// ---- speed -----------------------------------------------------------------

// Speed shares the distance-unit preference: nautical miles -> knots, statute
// miles -> mph, kilometres -> km/h. Planned speed is stored and exported in
// knots (the GPX/OpenCPN convention); these convert for display and entry.
inline QString speedUnitKey(DistanceUnit u) {
    switch (u) {
        case DistanceUnit::StatuteMiles: return QStringLiteral("mph");
        case DistanceUnit::Kilometers:   return QStringLiteral("km/h");
        case DistanceUnit::NauticalMiles: break;
    }
    return QStringLiteral("kn");
}

inline double speedFromKnots(double kts, DistanceUnit u) {
    switch (u) {
        case DistanceUnit::StatuteMiles: return kts * kNmToStatuteMiles;
        case DistanceUnit::Kilometers:   return kts * kNmToKilometers;
        case DistanceUnit::NauticalMiles: break;
    }
    return kts;
}
inline double knotsFromSpeed(double value, DistanceUnit u) {
    switch (u) {
        case DistanceUnit::StatuteMiles: return value / kNmToStatuteMiles;
        case DistanceUnit::Kilometers:   return value / kNmToKilometers;
        case DistanceUnit::NauticalMiles: break;
    }
    return value;
}

// ---- short distances (feet / metres) ---------------------------------------

// Thresholds like the track-point spacing are tens-of-metres scale, where a
// nautical mile figure ("0.025 nm") is unreadable. They are stored in nautical
// miles like every other distance, but entered and shown in feet or metres. There
// is no separate preference for these, so the depth unit — the app's only
// feet-vs-metres choice — selects which.
constexpr double kNmToMeters = 1852.0;
constexpr double kNmToFeet   = kNmToMeters * kMetersToFeet;   // ~6076.1 ft

inline double shortDistanceFromNm(double nm, DepthUnit u) {
    return nm * (u == DepthUnit::Meters ? kNmToMeters : kNmToFeet);
}
inline double nmFromShortDistance(double value, DepthUnit u) {
    return value / (u == DepthUnit::Meters ? kNmToMeters : kNmToFeet);
}
inline QString shortDistanceUnitKey(DepthUnit u) {
    return u == DepthUnit::Meters ? QStringLiteral("m") : QStringLiteral("ft");
}

// ---- coordinate (lat/lon) display format -----------------------------------

inline QString angleFormatKey(AngleFormat u) {
    switch (u) {
        case AngleFormat::DegMinutes: return QStringLiteral("dm");
        case AngleFormat::DegMinSec:  return QStringLiteral("dms");
        case AngleFormat::DecimalDegrees: break;
    }
    return QStringLiteral("dd");
}
inline AngleFormat angleFormatFromKey(const QString& s,
                                      AngleFormat fallback = AngleFormat::DecimalDegrees) {
    if (s == QStringLiteral("dm"))  return AngleFormat::DegMinutes;
    if (s == QStringLiteral("dms")) return AngleFormat::DegMinSec;
    if (s == QStringLiteral("dd"))  return AngleFormat::DecimalDegrees;
    return fallback;
}
inline QString angleFormatLabel(AngleFormat u) {
    switch (u) {
        case AngleFormat::DegMinutes:
            return QStringLiteral("Degrees, decimal minutes (12° 34.567')");
        case AngleFormat::DegMinSec:
            return QStringLiteral("Degrees, minutes, seconds (12° 34' 56.7\")");
        case AngleFormat::DecimalDegrees: break;
    }
    return QStringLiteral("Decimal degrees (12.34567°)");
}

// ---- bearings (true vs magnetic) -------------------------------------------

inline QString bearingModeKey(BearingMode b) {
    return b == BearingMode::Magnetic ? QStringLiteral("magnetic")
                                      : QStringLiteral("true");
}
inline BearingMode bearingModeFromKey(const QString& s,
                                      BearingMode fallback = BearingMode::True) {
    if (s == QStringLiteral("magnetic")) return BearingMode::Magnetic;
    if (s == QStringLiteral("true"))     return BearingMode::True;
    return fallback;
}
inline QString bearingModeLabel(BearingMode b) {
    return b == BearingMode::Magnetic ? QStringLiteral("Magnetic")
                                      : QStringLiteral("True");
}

// Format a bearing/heading in degrees as a 3-digit compass value with a degree
// sign and a reference letter — "045°T" (true) or "127°M" (magnetic). The caller
// supplies the value already in the desired reference; `mode` only selects the
// suffix. The input is normalised to [0, 360).
inline QString formatBearing(double deg, BearingMode mode) {
    double d = std::fmod(deg, 360.0);
    if (d < 0.0) d += 360.0;
    int di = int(d + 0.5);            // round to whole degrees
    if (di >= 360) di -= 360;         // 359.7 rounds up to 360 -> 000
    const QChar D(0x00B0);
    const QChar ref = (mode == BearingMode::Magnetic) ? QLatin1Char('M') : QLatin1Char('T');
    return QStringLiteral("%1").arg(di, 3, 10, QLatin1Char('0')) + D + ref;
}

// Process-wide current coordinate format. An inline function-local static gives a
// single shared instance across translation units (header-only). The host seeds
// it from Settings at startup and updates it when the user changes the format, so
// display widgets can format coordinates without each holding a Settings pointer.
inline AngleFormat& coordFormatRef() {
    static AngleFormat fmt = AngleFormat::DecimalDegrees;
    return fmt;
}
inline AngleFormat coordFormat()            { return coordFormatRef(); }
inline void        setCoordFormat(AngleFormat f) { coordFormatRef() = f; }

// Format one signed degree value as latitude (N/S) or longitude (E/W) in the
// given format. Decimal degrees to 5 places (~1 m), decimal minutes to 3 places,
// seconds to 1 place; rounding carries cleanly (e.g. 59.9996' -> next degree).
inline QString formatAngle(double deg, bool isLat, AngleFormat fmt) {
    const QChar D(0x00B0);
    const bool neg = deg < 0.0;
    double a = neg ? -deg : deg;
    const QString hem = isLat ? (neg ? QStringLiteral("S") : QStringLiteral("N"))
                              : (neg ? QStringLiteral("W") : QStringLiteral("E"));
    if (fmt == AngleFormat::DegMinutes) {
        int d = int(a);
        double m = (a - d) * 60.0;
        double mR = std::floor(m * 1000.0 + 0.5) / 1000.0;   // round to 0.001'
        if (mR >= 60.0) { mR -= 60.0; d += 1; }
        QString mStr = QString::number(mR, 'f', 3);
        if (mR < 10.0) mStr.prepend(QLatin1Char('0'));
        return QString::number(d) + D + QLatin1Char(' ') + mStr + QStringLiteral("' ") + hem;
    }
    if (fmt == AngleFormat::DegMinSec) {
        int d = int(a);
        double mf = (a - d) * 60.0;
        int m = int(mf);
        double s = (mf - m) * 60.0;
        double sR = std::floor(s * 10.0 + 0.5) / 10.0;       // round to 0.1"
        if (sR >= 60.0) { sR -= 60.0; m += 1; }
        if (m >= 60)    { m -= 60;    d += 1; }
        QString mStr = (m < 10 ? QStringLiteral("0") : QString()) + QString::number(m);
        QString sStr = QString::number(sR, 'f', 1);
        if (sR < 10.0) sStr.prepend(QLatin1Char('0'));
        return QString::number(d) + D + QLatin1Char(' ') + mStr + QStringLiteral("' ")
             + sStr + QStringLiteral("\" ") + hem;
    }
    return QString::number(a, 'f', 5) + D + QLatin1Char(' ') + hem;
}
inline QString formatLatitude(double deg, AngleFormat f)  { return formatAngle(deg, true,  f); }
inline QString formatLongitude(double deg, AngleFormat f) { return formatAngle(deg, false, f); }
inline QString formatLatitude(double deg)  { return formatAngle(deg, true,  coordFormat()); }
inline QString formatLongitude(double deg) { return formatAngle(deg, false, coordFormat()); }

// Tolerant parse of a coordinate string back to signed decimal degrees. Accepts
// 1/2/3 numeric components (deg / deg+min / deg+min+sec), any separators, an
// optional sign, and an N/S/E/W hemisphere letter. *ok reports validity (parsed
// and within ±90 for lat / ±180 for lon). Handles every format formatAngle emits
// plus plain decimal entry.
inline double parseAngle(const QString& text, bool isLat, bool* ok = nullptr) {
    const QChar negHem = isLat ? QLatin1Char('S') : QLatin1Char('W');
    bool neg = text.contains(QLatin1Char('-'));
    if (text.contains(negHem, Qt::CaseInsensitive)) neg = true;

    QList<double> nums;
    QString cur;
    auto flush = [&] {
        if (cur.isEmpty()) return;
        bool k = false;
        const double d = cur.toDouble(&k);
        if (k) nums.append(d);
        cur.clear();
    };
    for (const QChar c : text) {
        if (c.isDigit() || c == QLatin1Char('.')) cur += c;
        else flush();
    }
    flush();
    if (nums.isEmpty()) { if (ok) *ok = false; return 0.0; }

    double a = nums.value(0);
    if (nums.size() >= 2) a += nums.value(1) / 60.0;
    if (nums.size() >= 3) a += nums.value(2) / 3600.0;
    double deg = neg ? -a : a;

    const double lim = isLat ? 90.0 : 180.0;
    if (ok) *ok = (deg >= -lim && deg <= lim);
    return deg;
}
inline double parseLatitude(const QString& s, bool* ok = nullptr)  { return parseAngle(s, true,  ok); }
inline double parseLongitude(const QString& s, bool* ok = nullptr) { return parseAngle(s, false, ok); }

} // namespace units
