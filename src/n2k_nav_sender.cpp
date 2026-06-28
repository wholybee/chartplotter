#include "n2k_nav_sender.hpp"
#include "nmea2000_client.hpp"
#include "nav_data_store.hpp"
#include "n2k_frame.hpp"

#include <QtGlobal>
#include <cmath>

namespace {
constexpr double kDeg2Rad   = 0.017453292519943295;
constexpr double kKn_to_Ms  = 1.0 / 1.9438444924406046;   // knots -> m/s
constexpr double kMPerNm    = 1852.0;

// Source address stamped on transmitted frames. The gateway typically claims
// its own address on the bus, but the field must carry a plausible value.
constexpr quint8 kTxSrc = 0x16;

double wrap360(double d) {
    d = std::fmod(d, 360.0);
    if (d < 0.0) d += 360.0;
    return d;
}

// Encode an angle in degrees to NMEA 2000's 16-bit 1e-4 radian unit.
quint16 angle16(double deg) {
    const double raw = std::lround(wrap360(deg) * kDeg2Rad / 1e-4);
    if (raw < 0.0 || raw >= 0xFFFF) return 0xFFFF;          // out of range -> N/A
    return quint16(raw);
}

// Append a waypoint name as an NMEA 2000 variable-length string (STRINGLAU):
// [total length incl. these 2 bytes][control: 1 = ASCII][chars…]. Must be called
// byte-aligned (it always is at our call sites).
void appendStringLau(N2kWriter& w, const QString& s) {
    const QByteArray a = s.toLatin1();
    QByteArray out;
    out.append(char(quint8(a.size() + 2)));
    out.append(char(0x01));                                  // 1 = ASCII
    out.append(a);
    w.appendBytes(out);
}

// ---- PGN 129283: Cross Track Error -------------------------------------------
N2kFrame buildXte(const NavigationData& n) {
    N2kFrame f;
    f.pgn = 129283; f.prio = 3; f.src = kTxSrc; f.dst = 0xFF;
    N2kWriter w;
    w.naU(8);                  // SID (not used)
    w.u(0, 4);                 // XTE mode: 0 = Autonomous
    w.naU(2);                  // reserved
    w.u(0, 2);                 // Navigation Terminated: 0 = No (we stop sending instead)
    // Signed XTE in 0.01 m. Sign convention here: positive = steer to port (the
    // 'L' steer direction), negative = steer to starboard. Flip the constant
    // below if a particular autopilot expects the opposite sense.
    double xteM = n.xteNm * kMPerNm;
    if (n.steerDirection == QLatin1Char('R')) xteM = -xteM;
    w.i(qint64(std::llround(xteM / 0.01)), 32);
    w.naU(16);                 // pad to the 8-byte single-frame length
    f.data = w.bytes();
    return f;
}

// ---- PGN 129284: Navigation Data ---------------------------------------------
N2kFrame buildNavData(const NavigationData& n) {
    N2kFrame f;
    f.pgn = 129284; f.prio = 3; f.src = kTxSrc; f.dst = 0xFF;
    N2kWriter w;
    w.naU(8);                                              // SID
    w.u(quint64(std::llround(n.rangeToDestNm * kMPerNm / 0.01)), 32);   // distance, 0.01 m
    w.u(n.bearingUnits == QLatin1Char('M') ? 1 : 0, 2);   // bearing ref: 0 True, 1 Magnetic
    w.u(n.perpendicularPassed  ? 1 : 0, 2);               // perpendicular crossed
    w.u(n.arrivalCircleEntered ? 1 : 0, 2);               // arrival circle entered
    w.u(0, 2);                                            // calculation type: 0 = Great Circle
    w.naU(32);                                            // ETA time (not computed)
    w.naU(16);                                            // ETA date (not computed)
    w.u(angle16(n.bearingOriginToDestDeg), 16);           // bearing, origin -> dest
    w.u(angle16(n.bearingPresentToDestDeg), 16);          // bearing, position -> dest
    w.u(0, 32);                                           // origin waypoint number
    w.u(1, 32);                                           // destination waypoint number
    w.i(qint64(std::llround(n.destinationLatDeg / 1e-7)), 32);
    w.i(qint64(std::llround(n.destinationLonDeg / 1e-7)), 32);
    w.i(qint64(std::llround(n.closingVelocityKn * kKn_to_Ms / 0.01)), 16);  // 0.01 m/s
    f.data = w.bytes();
    return f;
}

// ---- PGN 129285: Navigation - Route / WP Information -------------------------
N2kFrame buildRouteWp(const NavigationData& n) {
    N2kFrame f;
    f.pgn = 129285; f.prio = 6; f.src = kTxSrc; f.dst = 0xFF;
    N2kWriter w;
    const quint16 nItems = n.hasOrigin ? 2 : 1;
    w.u(0, 16);            // Start RPS# (index of first WP in this message)
    w.u(nItems, 16);       // number of waypoints that follow
    w.naU(16);             // Database ID
    w.naU(16);             // Route ID
    w.u(0, 3);             // Navigation direction in route: 0 = forward
    w.u(0, 3);             // Supplementary route/WP info: 0 = off
    w.naU(2);              // reserved
    appendStringLau(w, QString());   // Route name (unknown here)
    w.naU(8);              // reserved

    auto appendWp = [&](quint16 idx, const QString& name, double lat, double lon) {
        w.u(idx, 16);
        appendStringLau(w, name);
        w.i(qint64(std::llround(lat / 1e-7)), 32);
        w.i(qint64(std::llround(lon / 1e-7)), 32);
    };
    quint16 idx = 0;
    if (n.hasOrigin)
        appendWp(idx++, n.originWaypointId, n.originLatDeg, n.originLonDeg);
    appendWp(idx, n.destinationWaypointId, n.destinationLatDeg, n.destinationLonDeg);
    f.data = w.bytes();
    return f;
}
} // namespace

N2kNavSender::N2kNavSender(NavDataStore* store, Nmea2000Client* client,
                           QString ownSourceId, QObject* parent)
    : QObject(parent), store_(store), client_(client),
      ownSourceId_(std::move(ownSourceId)) {
    if (store_)
        connect(store_, &NavDataStore::navigationChanged,
                this, &N2kNavSender::onNavigationChanged);
}

void N2kNavSender::onNavigationChanged() {
    if (!store_ || !client_) return;
    const NavigationData& n = store_->navigation();

    // Decide whether we will actually put PGNs on the wire this tick, and publish
    // that so the nav display's transmit indicator reflects reality: a route must
    // be active, the loop guard must not be suppressing us, and the link must be
    // up.
    const bool suppressed = !ownSourceId_.isEmpty()
        && n.source.compare(ownSourceId_, Qt::CaseInsensitive) == 0;
    const bool transmitting = n.active && !suppressed && client_->canTransmit();
    store_->setNavTransmitting(ownSourceId_, transmitting);
    if (!transmitting) return;

    client_->transmit(buildXte(n));
    client_->transmit(buildNavData(n));
    client_->transmit(buildRouteWp(n));
}
