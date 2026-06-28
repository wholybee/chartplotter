#include "ais_target_store.hpp"
#include <QTimer>

QString aisNavStatusName(int code) {
    switch (code) {
        case 0:  return QStringLiteral("Under way using engine");
        case 1:  return QStringLiteral("At anchor");
        case 2:  return QStringLiteral("Not under command");
        case 3:  return QStringLiteral("Restricted manoeuvrability");
        case 4:  return QStringLiteral("Constrained by draught");
        case 5:  return QStringLiteral("Moored");
        case 6:  return QStringLiteral("Aground");
        case 7:  return QStringLiteral("Engaged in fishing");
        case 8:  return QStringLiteral("Under way sailing");
        case 14: return QStringLiteral("AIS-SART");
        case 15: return QStringLiteral("Undefined");
        default: return QStringLiteral("Reserved (%1)").arg(code);
    }
}

QString aisShipTypeName(int code) {
    // First-digit category (per ITU-R M.1371). Specific known codes get a name;
    // the rest fall back to the category bucket so the user gets context.
    if (code <= 0) return QStringLiteral("Not available");
    if (code == 30) return QStringLiteral("Fishing");
    if (code == 31) return QStringLiteral("Towing");
    if (code == 32) return QStringLiteral("Towing (long)");
    if (code == 33) return QStringLiteral("Dredging");
    if (code == 34) return QStringLiteral("Diving operations");
    if (code == 35) return QStringLiteral("Military operations");
    if (code == 36) return QStringLiteral("Sailing");
    if (code == 37) return QStringLiteral("Pleasure craft");
    if (code == 50) return QStringLiteral("Pilot vessel");
    if (code == 51) return QStringLiteral("Search and rescue");
    if (code == 52) return QStringLiteral("Tug");
    if (code == 53) return QStringLiteral("Port tender");
    if (code == 54) return QStringLiteral("Anti-pollution");
    if (code == 55) return QStringLiteral("Law enforcement");
    if (code == 58) return QStringLiteral("Medical transport");
    if (code >= 20 && code <= 29) return QStringLiteral("WIG (%1)").arg(code);
    if (code >= 40 && code <= 49) return QStringLiteral("High-speed craft (%1)").arg(code);
    if (code >= 60 && code <= 69) return QStringLiteral("Passenger (%1)").arg(code);
    if (code >= 70 && code <= 79) return QStringLiteral("Cargo (%1)").arg(code);
    if (code >= 80 && code <= 89) return QStringLiteral("Tanker (%1)").arg(code);
    if (code >= 90 && code <= 99) return QStringLiteral("Other (%1)").arg(code);
    return QStringLiteral("Type %1").arg(code);
}

QString aisClassName(AisClass cls) {
    switch (cls) {
        case AisClass::A:           return QStringLiteral("Class A");
        case AisClass::B:           return QStringLiteral("Class B");
        case AisClass::AtoN:        return QStringLiteral("Aid to Navigation");
        case AisClass::SarAircraft: return QStringLiteral("SAR aircraft");
        case AisClass::Sart:        return QStringLiteral("Distress beacon");
        case AisClass::Unknown:     break;
    }
    return QString();
}

QString aisAtonTypeName(int code) {
    switch (code) {
        case 0:  return QStringLiteral("Aid, type unspecified");
        case 1:  return QStringLiteral("Reference point");
        case 2:  return QStringLiteral("RACON");
        case 3:  return QStringLiteral("Fixed off-shore structure");
        case 5:  return QStringLiteral("Light, without sectors");
        case 6:  return QStringLiteral("Light, with sectors");
        case 7:  return QStringLiteral("Leading light front");
        case 8:  return QStringLiteral("Leading light rear");
        case 9:  return QStringLiteral("Beacon, cardinal N");
        case 10: return QStringLiteral("Beacon, cardinal E");
        case 11: return QStringLiteral("Beacon, cardinal S");
        case 12: return QStringLiteral("Beacon, cardinal W");
        case 13: return QStringLiteral("Beacon, port hand");
        case 14: return QStringLiteral("Beacon, starboard hand");
        case 15: return QStringLiteral("Beacon, preferred channel port");
        case 16: return QStringLiteral("Beacon, preferred channel starboard");
        case 17: return QStringLiteral("Beacon, isolated danger");
        case 18: return QStringLiteral("Beacon, safe water");
        case 19: return QStringLiteral("Beacon, special mark");
        case 20: return QStringLiteral("Cardinal mark N");
        case 21: return QStringLiteral("Cardinal mark E");
        case 22: return QStringLiteral("Cardinal mark S");
        case 23: return QStringLiteral("Cardinal mark W");
        case 24: return QStringLiteral("Port hand mark");
        case 25: return QStringLiteral("Starboard hand mark");
        case 26: return QStringLiteral("Preferred channel port");
        case 27: return QStringLiteral("Preferred channel starboard");
        case 28: return QStringLiteral("Isolated danger");
        case 29: return QStringLiteral("Safe water");
        case 30: return QStringLiteral("Special mark");
        case 31: return QStringLiteral("Light vessel / LANBY / rig");
        case 4:  return QStringLiteral("Reserved (%1)").arg(code);
        default: return QStringLiteral("Aid type %1").arg(code);
    }
}

QString aisDistressKind(quint32 mmsi) {
    switch (mmsi / 1000000) {
        case 970: return QStringLiteral("AIS-SART");
        case 972: return QStringLiteral("AIS-MOB");
        case 974: return QStringLiteral("AIS-EPIRB");
        default:  return QString();
    }
}

QString aisFormatTcpa(double seconds) {
    const bool  neg   = seconds < 0;
    const qint64 total = qRound64(neg ? -seconds : seconds);
    const qint64 h = total / 3600;
    const qint64 m = (total % 3600) / 60;
    const qint64 s = total % 60;

    QString out;
    if (h > 0) out += QStringLiteral("%1h ").arg(h, 2, 10, QChar('0'));
    if (m > 0) out += QStringLiteral("%1m ").arg(m, 2, 10, QChar('0'));
    out += QStringLiteral("%1s").arg(s, 2, 10, QChar('0'));
    return neg ? QChar('-') + out : out;
}

namespace {
// Copy an optional into the target only when the report actually supplies it,
// so a report that omits a field doesn't wipe a previously known value.
template <class T>
void applyIf(std::optional<T>& dst, const std::optional<T>& src) {
    if (src.has_value()) dst = src;
}
} // namespace

AisTargetStore::AisTargetStore(QObject* parent) : QObject(parent) {
    tickTimer_ = new QTimer(this);
    tickTimer_->setInterval(1000);   // 1 Hz is plenty for AIS aging
    connect(tickTimer_, &QTimer::timeout, this, &AisTargetStore::tick);
    tickTimer_->start();
}

const AisTarget* AisTargetStore::target(quint32 mmsi) const {
    auto it = targets_.constFind(mmsi);
    return it == targets_.constEnd() ? nullptr : &it.value();
}

AisTarget& AisTargetStore::touch(quint32 mmsi, AisClass cls, const QString& source) {
    AisTarget& t = targets_[mmsi];   // inserts a default target if new
    t.mmsi = mmsi;
    // Distress beacons (AIS-SART 970, MOB 972, EPIRB-AIS 974) transmit ordinary
    // Class A position reports; reclassify by MMSI so they get the SART symbol
    // regardless of which transport decoded them.
    if (cls != AisClass::Unknown && !aisDistressKind(mmsi).isEmpty())
        cls = AisClass::Sart;
    if (cls != AisClass::Unknown) t.cls = cls;
    t.source = source;
    t.lastUpdateUtc = QDateTime::currentDateTimeUtc();
    t.ageSeconds = 0.0;
    t.freshness = AisFreshness::Current;
    return t;
}

void AisTargetStore::publishAisPosition(const AisPositionReport& r, const QString& source) {
    if (r.mmsi == 0) return;
    AisTarget& t = touch(r.mmsi, r.cls, source);
    applyIf(t.latitudeDeg,    r.latitudeDeg);
    applyIf(t.longitudeDeg,   r.longitudeDeg);
    applyIf(t.cogDegTrue,     r.cogDegTrue);
    applyIf(t.sogKnots,       r.sogKnots);
    applyIf(t.headingDegTrue, r.headingDegTrue);
    applyIf(t.rotDegPerMin,   r.rotDegPerMin);
    applyIf(t.altitudeMeters, r.altitudeMeters);
    if (r.navStatus != AisNavStatus::Undefined) t.navStatus = r.navStatus;
    emit targetUpdated(r.mmsi);
}

void AisTargetStore::publishAisStatic(const AisStaticData& d, const QString& source) {
    if (d.mmsi == 0) return;
    AisTarget& t = touch(d.mmsi, d.cls, source);
    if (!d.name.isEmpty())        t.name = d.name;
    if (!d.callSign.isEmpty())    t.callSign = d.callSign;
    if (!d.destination.isEmpty()) t.destination = d.destination;
    applyIf(t.shipType,      d.shipType);
    applyIf(t.imoNumber,     d.imoNumber);
    applyIf(t.draughtMeters, d.draughtMeters);
    if (d.dimensions.known()) t.dimensions = d.dimensions;
    applyIf(t.atonType, d.atonType);
    if (d.atonVirtual)     t.atonVirtual     = *d.atonVirtual;
    if (d.atonOffPosition) t.atonOffPosition = *d.atonOffPosition;
    emit targetUpdated(d.mmsi);
}

void AisTargetStore::setRangeMeters(quint32 mmsi, std::optional<double> rangeMeters) {
    auto it = targets_.find(mmsi);
    if (it == targets_.end()) return;
    it->rangeMeters = rangeMeters;
    emit targetUpdated(mmsi);
}

void AisTargetStore::setCpaTcpa(quint32 mmsi, std::optional<double> cpaMeters,
                                std::optional<double> tcpaSeconds,
                                std::optional<GeoPos> ownshipAtCpa,
                                std::optional<GeoPos> targetAtCpa) {
    auto it = targets_.find(mmsi);
    if (it == targets_.end()) return;
    it->cpaMeters = cpaMeters;
    it->tcpaSeconds = tcpaSeconds;
    it->cpaOwnshipPos = ownshipAtCpa;
    it->cpaTargetPos  = targetAtCpa;
    emit targetUpdated(mmsi);
}

void AisTargetStore::setStaleSeconds(double s) { staleSeconds_ = s; }
void AisTargetStore::setLostSeconds(double s)  { lostSeconds_ = s; }

void AisTargetStore::tick() {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QList<quint32> expired;

    for (auto it = targets_.begin(); it != targets_.end(); ++it) {
        AisTarget& t = it.value();
        t.ageSeconds = t.lastUpdateUtc.isValid()
            ? t.lastUpdateUtc.msecsTo(now) / 1000.0 : 0.0;

        AisFreshness f;
        if      (t.ageSeconds < staleSeconds_) f = AisFreshness::Current;
        else if (t.ageSeconds < lostSeconds_)  f = AisFreshness::Stale;
        else                                   f = AisFreshness::Lost;

        if (f == AisFreshness::Lost) {
            expired << it.key();
        } else if (f != t.freshness) {
            t.freshness = f;
            emit targetUpdated(it.key());
        }
    }

    for (quint32 mmsi : expired) {
        targets_.remove(mmsi);
        emit targetExpired(mmsi);
    }
}
