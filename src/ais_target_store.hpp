#pragma once
#include <QObject>
#include <QString>
#include <QHash>
#include <QDateTime>
#include <optional>

class QTimer;

// AIS transponder class / target kind.
//   A / B        — vessels (Class A = SOLAS, reports more than Class B).
//   AtoN         — Aid to Navigation (buoy, beacon, lighthouse; msg 21).
//   SarAircraft  — Search-and-rescue aircraft position report (msg 9).
//   Sart         — distress beacon: AIS-SART / MOB / EPIRB-AIS (msg 1 from a
//                  970/972/974-range MMSI).
enum class AisClass { Unknown, A, B, AtoN, SarAircraft, Sart };

// Per-target freshness: a contact ages out of the display when it stops being
// heard. (Distinct from ownship NavFreshness — AIS uses minutes, not seconds.)
enum class AisFreshness { Current, Stale, Lost };

// Human-readable label for an AIS nav status code.
QString aisNavStatusName(int code);
// Human-readable label for an AIS ship & cargo type code (best-effort
// categorisation; falls back to the numeric code).
QString aisShipTypeName(int code);
// Human-readable label for an AIS target class ("Class A", "Aid to Navigation",
// "SAR aircraft", "AIS-SART", ...); empty for AisClass::Unknown.
QString aisClassName(AisClass cls);
// Human-readable name for an AtoN aid-type code (0..31, ITU-R M.1371 Table 75).
QString aisAtonTypeName(int code);
// For a distress-beacon MMSI, the device kind ("AIS-SART", "AIS-MOB",
// "AIS-EPIRB"); empty for any other MMSI.
QString aisDistressKind(quint32 mmsi);

// Format a TCPA (seconds) as "00h 00m 00s", dropping the hours or minutes field
// whenever that field's value is zero (seconds are always shown). A negative
// value — the contact is opening, i.e. CPA already passed — is prefixed with '-'.
QString aisFormatTcpa(double seconds);

// Standard AIS navigational status codes (0..15). Class B has no status.
enum class AisNavStatus {
    UnderWayEngine = 0, AtAnchor = 1, NotUnderCommand = 2,
    RestrictedManoeuvrability = 3, ConstrainedByDraught = 4, Moored = 5,
    Aground = 6, Fishing = 7, UnderWaySailing = 8,
    Reserved9 = 9, Reserved10 = 10, Reserved11 = 11, Reserved12 = 12,
    Reserved13 = 13, AisSart = 14, Undefined = 15
};

// A geographic point. Used for the projected CPA endpoints: where each vessel
// will be at the moment of closest approach.
struct GeoPos {
    double latDeg = 0.0;
    double lonDeg = 0.0;
};

// AIS reports a vessel's size as distances (metres) from the position reference
// point to bow (A), stern (B), port (C) and starboard (D).
struct AisDimensions {
    double toBow = 0, toStern = 0, toPort = 0, toStarboard = 0;
    bool   known() const { return toBow || toStern || toPort || toStarboard; }
    double lengthMeters() const { return toBow + toStern; }
    double beamMeters()   const { return toPort + toStarboard; }
};

// A tracked AIS vessel. Built up from position reports (dynamic) and static /
// voyage reports; fields are absent until a report supplies them.
struct AisTarget {
    quint32 mmsi = 0;
    AisClass cls = AisClass::Unknown;

    // Static / voyage (Class A/B details).
    QString name;
    QString callSign;
    QString destination;            // Class A
    std::optional<int>    shipType;        // AIS ship & cargo type code
    std::optional<int>    imoNumber;       // Class A
    std::optional<double> draughtMeters;   // Class A
    AisDimensions dimensions;

    // Aid to Navigation (Class AtoN, msg 21).
    std::optional<int> atonType;           // aid-type code 0..31
    bool atonVirtual    = false;           // virtual aid (no physical structure)
    bool atonOffPosition = false;          // reported off its charted position

    // Dynamic.
    std::optional<double> latitudeDeg, longitudeDeg;
    std::optional<double> cogDegTrue, sogKnots;
    std::optional<double> headingDegTrue;
    std::optional<double> rotDegPerMin;    // rate of turn (+ = starboard)
    std::optional<double> altitudeMeters;  // SAR aircraft altitude (msg 9)
    AisNavStatus navStatus = AisNavStatus::Undefined;

    // Computed by a collision component (not from AIS messages).
    std::optional<double> rangeMeters;     // current distance to ownship
    std::optional<double> cpaMeters;       // closest point of approach
    std::optional<double> tcpaSeconds;     // time to CPA (< 0 = opening)
    // Where each vessel will be at TCPA (set with cpaMeters/tcpaSeconds; absent
    // when TCPA has no finite solution). Drives the CPA encounter graphics.
    std::optional<GeoPos> cpaOwnshipPos;   // ownship at the moment of CPA
    std::optional<GeoPos> cpaTargetPos;    // this target at the moment of CPA

    // Provenance / freshness.
    QString   source;
    QDateTime lastUpdateUtc;
    double    ageSeconds = 0.0;
    AisFreshness freshness = AisFreshness::Current;

    bool hasPosition() const { return latitudeDeg && longitudeDeg; }
};

// Inputs a source decodes from AIS messages. Position reports carry dynamic
// data (msg 1/2/3 Class A, 18/19 Class B); static reports carry identity and
// dimensions (msg 5 Class A, 24 Class B).
struct AisPositionReport {
    quint32 mmsi = 0;
    AisClass cls = AisClass::Unknown;
    std::optional<double> latitudeDeg, longitudeDeg;
    std::optional<double> cogDegTrue, sogKnots;
    std::optional<double> headingDegTrue;
    std::optional<double> rotDegPerMin;
    std::optional<double> altitudeMeters;   // SAR aircraft (msg 9)
    AisNavStatus navStatus = AisNavStatus::Undefined;
};

struct AisStaticData {
    quint32 mmsi = 0;
    AisClass cls = AisClass::Unknown;
    QString name, callSign, destination;
    std::optional<int>    shipType, imoNumber;
    std::optional<double> draughtMeters;
    AisDimensions dimensions;
    // Aid to Navigation (msg 21) identity, published alongside its position.
    std::optional<int>  atonType;
    std::optional<bool> atonVirtual;
    std::optional<bool> atonOffPosition;
};

// Stable API AIS sources (e.g. an AIS decoder plugin) call to publish targets.
// Mirrors INavDataPublisher: the core owns the data, sources only publish.
class IAisPublisher {
public:
    virtual ~IAisPublisher() = default;
    virtual void publishAisPosition(const AisPositionReport& report,
                                    const QString& source) = 0;
    virtual void publishAisStatic(const AisStaticData& data,
                                  const QString& source) = 0;
};

// Central AIS target store, keyed by MMSI. Sources publish via IAisPublisher;
// consumers (AIS overlay, target list) subscribe to targetUpdated/targetExpired
// and read targets(). A background tick ages each target Current -> Stale ->
// Lost; a Lost target is removed and targetExpired() is emitted.
class AisTargetStore : public QObject, public IAisPublisher {
    Q_OBJECT
public:
    explicit AisTargetStore(QObject* parent = nullptr);

    const QHash<quint32, AisTarget>& targets() const { return targets_; }
    const AisTarget* target(quint32 mmsi) const;     // nullptr if not tracked
    int  count() const { return targets_.size(); }

    double staleSeconds() const { return staleSeconds_; }
    double lostSeconds()  const { return lostSeconds_; }

    // IAisPublisher -----------------------------------------------------------
    void publishAisPosition(const AisPositionReport& report, const QString& source) override;
    void publishAisStatic(const AisStaticData& data, const QString& source) override;

    // Range/CPA/TCPA computed elsewhere (ownship vs target) and attached to a
    // target. Distance is separate because it is known whenever both positions
    // are (even when a target reports no course/speed and CPA can't be solved).
    void setRangeMeters(quint32 mmsi, std::optional<double> rangeMeters);
    // The optional endpoints are each vessel's projected position at TCPA;
    // omitting them clears any previously attached pair.
    void setCpaTcpa(quint32 mmsi, std::optional<double> cpaMeters,
                    std::optional<double> tcpaSeconds,
                    std::optional<GeoPos> ownshipAtCpa = std::nullopt,
                    std::optional<GeoPos> targetAtCpa  = std::nullopt);

public slots:
    void setStaleSeconds(double s);
    void setLostSeconds(double s);

signals:
    void targetUpdated(quint32 mmsi);    // added, refreshed, or freshness changed
    void targetExpired(quint32 mmsi);    // aged out and removed from the store

private slots:
    void tick();

private:
    AisTarget& touch(quint32 mmsi, AisClass cls, const QString& source);  // upsert + refresh

    QHash<quint32, AisTarget> targets_;
    double  staleSeconds_ = 360.0;    // greyed after this with no message (6 min)
    double  lostSeconds_  = 720.0;    // removed after this (12 min)
    QTimer* tickTimer_ = nullptr;
};
