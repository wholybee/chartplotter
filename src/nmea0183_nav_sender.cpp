#include "nmea0183_nav_sender.hpp"
#include "nmea0183_client.hpp"
#include "nav_data_store.hpp"
#include "geo_nav.hpp"

#include <QStringList>
#include <QDateTime>
#include <cmath>

namespace {
// Talker id for chartplotter-originated sentences ("EC" = electronic chart
// system). Consumers match on the 3-letter sentence type, not the talker.
const QString kTalker = QStringLiteral("EC");

// Wrap a comma-joined sentence body (address + fields, no '$') with the leading
// '$', the XOR checksum, and CR/LF.
QByteArray finish(const QString& body) {
    const QByteArray b = body.toLatin1();
    unsigned char x = 0;
    for (char c : b) x ^= static_cast<unsigned char>(c);
    QByteArray out;
    out.reserve(b.size() + 6);
    out += '$';
    out += b;
    out += '*';
    out += QByteArray::number(x, 16).rightJustified(2, '0').toUpper();
    out += "\r\n";
    return out;
}

// ddmm.mmmm (latitude) / dddmm.mmmm (longitude), zero-padded.
QString fmtDegMin(double deg, int degWidth) {
    const double a = std::abs(deg);
    const int d = int(a);
    const double minutes = (a - d) * 60.0;
    return QString(QStringLiteral("%1%2"))
        .arg(d, degWidth, 10, QChar('0'))
        .arg(minutes, 7, 'f', 4, QChar('0'));
}
QString fmtLat(double lat) { return fmtDegMin(lat, 2); }
QString fmtLon(double lon) { return fmtDegMin(lon, 3); }
QChar   latHemi(double lat) { return lat < 0.0 ? QLatin1Char('S') : QLatin1Char('N'); }
QChar   lonHemi(double lon) { return lon < 0.0 ? QLatin1Char('W') : QLatin1Char('E'); }

QString num1(double v) { return QString::number(v, 'f', 1); }

// Bearing/heading: zero-padded to three integer digits with one decimal
// ("011.0", "123.4") — the conventional NMEA degrees format autopilots expect.
QString brg(double deg) { return QString::asprintf("%05.1f", deg); }

// XTE field: capped at 9.99 nm as the sentences specify, two decimals.
QString xteField(double nm) { return QString::number(std::min(nm, 9.99), 'f', 2); }

// Strip characters that would break the comma/checksum framing from a waypoint id.
QString cleanId(const QString& id) {
    QString s = id;
    s.replace(QLatin1Char(','), QLatin1Char(' '));
    s.replace(QLatin1Char('*'), QLatin1Char(' '));
    s.remove(QLatin1Char('$'));
    return s;
}
}  // namespace

Nmea0183NavSender::Nmea0183NavSender(NavDataStore* store, Nmea0183Client* client,
                                     QString ownSourceId, QObject* parent)
    : QObject(parent), store_(store), client_(client),
      ownSourceId_(std::move(ownSourceId)) {
    if (store_)
        connect(store_, &NavDataStore::navigationChanged,
                this, &Nmea0183NavSender::onNavigationChanged);
}

void Nmea0183NavSender::onNavigationChanged() {
    if (!store_ || !client_) return;
    const NavigationData& n = store_->navigation();

    // Decide whether we will actually put sentences on the wire this tick, and
    // publish that so the nav display's transmit indicator reflects reality: a
    // route must be active, the loop guard must not be suppressing us, and the
    // link must be up.
    const bool suppressed = !ownSourceId_.isEmpty()
        && n.source.compare(ownSourceId_, Qt::CaseInsensitive) == 0;
    const bool transmitting = n.active && !suppressed && client_->canTransmit();
    store_->setNavTransmitting(ownSourceId_, transmitting);
    if (!transmitting) return;

    const OwnshipState& os = store_->ownship();
    const bool haveFix = os.latitudeDeg.valid() && os.longitudeDeg.valid();
    const bool haveSog = os.sogKnots.valid();
    const bool haveCog = os.cogDegTrue.valid();
    // Magnetic variation (east positive): prefer a published value, else derive
    // it from true vs magnetic heading, else 0. Always sent so an autopilot can
    // convert RMB's true bearing to magnetic.
    double variationDeg = 0.0;
    if (os.variationDeg.valid())
        variationDeg = os.variationDeg.value;
    else if (os.headingDegTrue.valid() && os.headingDegMag.valid())
        variationDeg = os.headingDegTrue.value - os.headingDegMag.value;

    const QString xte    = xteField(n.xteNm);
    const QString steer  = QString(n.steerDirection);
    const QString unit   = QString(n.bearingUnits);
    const QString mode   = QString(n.faaMode);
    const QString destId = cleanId(n.destinationWaypointId);
    const QString origId = cleanId(n.originWaypointId);
    const QChar arrival  = (n.arrivalCircleEntered || n.perpendicularPassed)
                               ? QLatin1Char('A') : QLatin1Char('V');

    // ---- APB: Autopilot Sentence B -----------------------------------------
    {
        QStringList f;
        f << (kTalker + QStringLiteral("APB"))
          << QStringLiteral("A") << QStringLiteral("A")   // status: OK, cycle-lock OK
          << xte << steer << QStringLiteral("N")
          << (n.arrivalCircleEntered ? QStringLiteral("A") : QStringLiteral("V"))
          << (n.perpendicularPassed  ? QStringLiteral("A") : QStringLiteral("V"))
          << brg(n.bearingOriginToDestDeg) << unit
          << destId
          << brg(n.bearingPresentToDestDeg) << unit
          << brg(n.headingToSteerDeg) << unit;
        // NB: no trailing FAA mode indicator. Older autopilots (e.g. Raymarine)
        // parse APB positionally and expect the heading-to-steer M/T field to be
        // last; an extra field after it makes them reject the sentence, so they
        // never get a steering command and keep prompting. Matches OpenCPN.
        client_->transmit(finish(f.join(QLatin1Char(','))));
    }

    // ---- XTE: Cross-Track Error --------------------------------------------
    // Many autopilots require XTE alongside APB/RMB to enter and hold track mode.
    // Like APB, omit the trailing FAA mode field for autopilot compatibility.
    {
        QStringList f;
        f << (kTalker + QStringLiteral("XTE"))
          << QStringLiteral("A") << QStringLiteral("A")
          << xte << steer << QStringLiteral("N");
        client_->transmit(finish(f.join(QLatin1Char(','))));
    }

    // ---- RMB: Recommended Minimum Navigation Information --------------------
    {
        // RMB bearing to destination is degrees true; compute it from the present
        // position so it is independent of the True/Magnetic display preference.
        const QString brgTrue = haveFix
            ? brg(geonav::initialBearingDeg(os.latitudeDeg.value, os.longitudeDeg.value,
                                            n.destinationLatDeg, n.destinationLonDeg))
            : QString();
        QStringList f;
        f << (kTalker + QStringLiteral("RMB"))
          << (haveFix ? QStringLiteral("A") : QStringLiteral("V"))
          << xte << steer
          << origId << destId
          << fmtLat(n.destinationLatDeg) << QString(latHemi(n.destinationLatDeg))
          << fmtLon(n.destinationLonDeg) << QString(lonHemi(n.destinationLonDeg))
          << num1(n.rangeToDestNm) << brgTrue << num1(n.closingVelocityKn)
          << QString(arrival)
          << mode;
        client_->transmit(finish(f.join(QLatin1Char(','))));
    }

    // ---- RMC: Recommended Minimum Specific GNSS Data -----------------------
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QStringList f;
        f << (kTalker + QStringLiteral("RMC"))
          << now.time().toString(QStringLiteral("HHmmss.zzz")).left(9)   // hhmmss.ss
          << (haveFix ? QStringLiteral("A") : QStringLiteral("V"))
          << (haveFix ? fmtLat(os.latitudeDeg.value)  : QString())
          << (haveFix ? QString(latHemi(os.latitudeDeg.value))  : QString())
          << (haveFix ? fmtLon(os.longitudeDeg.value) : QString())
          << (haveFix ? QString(lonHemi(os.longitudeDeg.value)) : QString())
          << (haveSog ? num1(os.sogKnots.value)   : QString())
          << (haveCog ? brg(os.cogDegTrue.value) : QString())
          << now.date().toString(QStringLiteral("ddMMyy"))
          // Always populate variation + hemisphere (0.0,E when unknown). An
          // autopilot steering off RMB's true bearing needs this field present to
          // convert to magnetic; an empty field stops it engaging track mode.
          << num1(std::abs(variationDeg))
          << QString(variationDeg < 0.0 ? QLatin1Char('W') : QLatin1Char('E'))
          << mode;
        client_->transmit(finish(f.join(QLatin1Char(','))));
    }
}
