#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QChar>
#include <QDateTime>
#include <QHash>
#include <optional>
#include "host_export.hpp"

class QTimer;

// Freshness of a single navigation value, derived from its age against the
// stale/invalid thresholds.
enum class NavFreshness { Fresh, Stale, Invalid };

// Provenance a publisher supplies when it sets a value: which source produced
// it and when. The store derives age/freshness from this.
struct NavValueMeta {
    QString   source;              // e.g. "nmea0183", "nmea2000", "signalk"
    QDateTime timestampUtc;        // when the source produced the value
};

// A single navigation value with its own provenance and freshness, so each
// field ages out independently and may come from a different source — e.g.
// position from NMEA 0183 while depth/wind arrive from NMEA 2000.
struct NavValue {
    double       value = 0.0;
    QString      source;                       // empty until first set
    QDateTime    timestampUtc;                 // invalid until first set
    double       ageSeconds = 0.0;             // maintained by the store
    NavFreshness freshness = NavFreshness::Invalid;

    bool   valid() const { return freshness != NavFreshness::Invalid; }
    bool   stale() const { return freshness == NavFreshness::Stale; }
    double valueOr(double fallback) const { return valid() ? value : fallback; }
};

// Live ownship navigation state: one self-describing value per field. A field
// whose freshness is Invalid is either never set or has aged out; consumers
// treat both as "not available". See ProjectSpec.md for the Signal K mapping.
struct OwnshipState {
    NavValue latitudeDeg;
    NavValue longitudeDeg;
    NavValue cogDegTrue;             // course over ground (true)
    NavValue sogKnots;               // speed over ground
    NavValue waterSpeedKnots;        // speed through the water
    NavValue headingDegTrue;
    NavValue headingDegMag;
    NavValue variationDeg;
    NavValue depthMeters;
    // Wind. Apparent and true are distinct quantities (relative to the bow);
    // true wind direction is geographic (relative to true north).
    NavValue apparentWindAngleDeg;   // relative to bow, 0..360 (0 = ahead)
    NavValue apparentWindSpeedKnots;
    NavValue trueWindAngleDeg;       // relative to bow, 0..360
    NavValue trueWindSpeedKnots;
    NavValue trueWindDirectionDeg;   // geographic (relative to true north)
};

// Computed route-following output, mirroring the fields of the NMEA 0183 APB and
// RMB sentences. Produced by RouteNavigator while the user is navigating a route
// and stored here for consumers. These are derived values (not arbitrated sensor
// inputs), so they live outside OwnshipState and update via setNavigationData().
//
// When `active` is false the struct is at its defaults and no route is being
// followed. Bearings are in the user-selected reference (true or magnetic), with
// `bearingUnits` recording which. All distances are nautical miles.
struct NavigationData {
    bool    active = false;                       // navigating a route

    // Who produced this navigation solution: "route-navigator" when computed
    // internally, or a link id (e.g. "nmea0183") if the navigation data was
    // instead received over that link and parsed into the store. An NMEA
    // transmitter uses this to avoid echoing APB/RMB back out the same link they
    // arrived on (loop guard).
    QString source;

    double  xteNm = 0.0;                           // cross-track error magnitude
    QChar   steerDirection = QLatin1Char('L');     // 'L' or 'R' to regain track
    QChar   xteUnits = QLatin1Char('N');           // always 'N' (nautical miles)

    bool    arrivalCircleEntered = false;          // within arrival radius of dest
    bool    perpendicularPassed  = false;          // crossed the perpendicular at dest

    double  bearingOriginToDestDeg = 0.0;          // previous -> next waypoint
    QChar   bearingUnits = QLatin1Char('T');       // 'T' (true) or 'M' (magnetic)

    QString destinationWaypointId;                 // name or number of next waypoint
    double  bearingPresentToDestDeg = 0.0;         // present position -> next waypoint
    double  headingToSteerDeg = 0.0;               // == bearingPresentToDest (for now)
    QString originWaypointId;                      // name or number of previous waypoint

    // Origin (previous) waypoint position. Set only when there is an origin leg
    // (a single-point route, or "head to the first point", has none). Carried so
    // NMEA 2000 PGN 129285 can list the active leg's two waypoints.
    bool    hasOrigin = false;
    double  originLatDeg = 0.0;
    double  originLonDeg = 0.0;

    double  destinationLatDeg = 0.0;
    double  destinationLonDeg = 0.0;
    double  rangeToDestNm = 0.0;                    // distance to next waypoint
    double  closingVelocityKn = 0.0;                // VMG toward next waypoint

    QChar   faaStatus = QLatin1Char('A');          // 'A' while active
    QChar   faaMode   = QLatin1Char('A');          // 'A' while active
};

// Stable API publishers (built-in or future dynamic plugins) call to publish
// navigation updates. Each call carries its own meta, so different fields can
// originate from different sources and age independently. The core owns the
// data and computes freshness; publishers never touch shared state directly.
class INavDataPublisher {
public:
    virtual ~INavDataPublisher() = default;
    virtual void publishOwnshipPosition(double latDeg, double lonDeg,
                                        const NavValueMeta& meta) = 0;
    virtual void publishCogSog(double cogDegTrue, double sogKnots,
                               const NavValueMeta& meta) = 0;
    // Heading: either component may be absent (e.g. HDT gives true only).
    virtual void publishHeading(std::optional<double> headingDegTrue,
                                std::optional<double> headingDegMag,
                                const NavValueMeta& meta) = 0;
    virtual void publishVariation(double variationDeg, const NavValueMeta& meta) = 0;
    virtual void publishDepth(double depthMeters, const NavValueMeta& meta) = 0;
    virtual void publishWaterSpeed(double knots, const NavValueMeta& meta) = 0;
    // Apparent / true wind angle (relative to the bow) + speed.
    virtual void publishApparentWind(double speedKnots, double angleDeg,
                                     const NavValueMeta& meta) = 0;
    virtual void publishTrueWind(double speedKnots, double angleDeg,
                                 const NavValueMeta& meta) = 0;
    // True wind direction (geographic) + speed.
    virtual void publishTrueWindDirection(double directionDeg, double speedKnots,
                                          const NavValueMeta& meta) = 0;
};

// Central navigation data store. Single source of truth for live nav state;
// publishers call publish*, consumers subscribe to ownshipChanged().
//
// Per-value staleness: a background tick ages every value and transitions it
// Fresh -> Stale -> Invalid as the thresholds elapse. ownshipChanged() fires on
// any publish and on any freshness transition, so consumers re-read and can
// render each value by its own freshness.
//
// Source arbitration: each value is taken from the highest-priority source. A
// lower-priority source can only overwrite a value once the current one becomes
// Invalid (aged out) — i.e. priority with fall-back. Priority is the ordered
// source-id list set via setSourcePriority() (highest first).
class HOST_EXPORT NavDataStore : public QObject, public INavDataPublisher {
    Q_OBJECT
public:
    explicit NavDataStore(QObject* parent = nullptr);

    const OwnshipState& ownship() const { return ownship_; }
    // Freshness of the position fix (drives the ownship symbol).
    NavFreshness positionFreshness() const { return ownship_.latitudeDeg.freshness; }

    // Route-following output (APB/RMB). Read current state and connect to
    // navigationChanged() to subscribe. Updated by RouteNavigator.
    const NavigationData& navigation() const { return navigation_; }
    double staleSeconds()   const { return staleSeconds_; }
    double invalidSeconds() const { return invalidSeconds_; }

    // Navigation transmit status, one flag per output link. A per-link NavSender
    // sets this each navigation tick: true while it is actually putting the
    // route's nav sentences/PGNs on the wire — i.e. a route is active, the link
    // is connected, and the loop guard isn't suppressing it. The nav display
    // reads it to colour its transmit indicators (so they reflect real link
    // state, not just the loop guard). Keyed by link source id ("nmea0183",
    // "nmea2000"); unknown ids read as false (nothing transmitting).
    bool navTransmitting(const QString& sourceId) const {
        return navTransmitting_.value(sourceId, false);
    }

    // INavDataPublisher --------------------------------------------------
    void publishOwnshipPosition(double latDeg, double lonDeg,
                                const NavValueMeta& meta) override;
    void publishCogSog(double cogDegTrue, double sogKnots,
                       const NavValueMeta& meta) override;
    void publishHeading(std::optional<double> headingDegTrue,
                        std::optional<double> headingDegMag,
                        const NavValueMeta& meta) override;
    void publishVariation(double variationDeg, const NavValueMeta& meta) override;
    void publishDepth(double depthMeters, const NavValueMeta& meta) override;
    void publishWaterSpeed(double knots, const NavValueMeta& meta) override;
    void publishApparentWind(double speedKnots, double angleDeg,
                             const NavValueMeta& meta) override;
    void publishTrueWind(double speedKnots, double angleDeg,
                         const NavValueMeta& meta) override;
    void publishTrueWindDirection(double directionDeg, double speedKnots,
                                  const NavValueMeta& meta) override;

public slots:
    void setStaleSeconds(double s);
    void setInvalidSeconds(double s);
    // Ordered source ids, highest priority first. Takes effect on subsequent
    // publishes (values switch as sources refresh / age out).
    void setSourcePriority(const QStringList& orderedSourceIds);

    // Route-following output. setNavigationData replaces the whole struct and
    // emits navigationChanged(); clearNavigation resets it to inactive defaults.
    void setNavigationData(const NavigationData& d);
    void clearNavigation();

    // Record a link's navigation transmit status (see navTransmitting()). Called
    // by the per-link NavSenders each navigation tick; emits
    // navTransmitStateChanged() only when a link's flag actually changes.
    void setNavTransmitting(const QString& sourceId, bool on);

signals:
    void ownshipChanged();            // a value or a freshness transitioned
    void navigationChanged();         // route-following output updated
    void navTransmitStateChanged();   // a link's transmit status flipped

private slots:
    void tick();

private:
    bool setValue(NavValue& v, double value, const NavValueMeta& meta);  // true if accepted
    bool accept(const NavValue& current, const QString& source) const;   // arbitration
    int  rank(const QString& source) const;             // priority index (lower = higher)
    bool ageValue(NavValue& v, const QDateTime& now);   // true if freshness changed
    bool recompute();                                   // age all; true if any changed

    OwnshipState ownship_;
    NavigationData navigation_;
    double staleSeconds_   = 5.0;
    double invalidSeconds_ = 30.0;
    QStringList sourcePriority_;     // highest priority first
    QHash<QString, bool> navTransmitting_;   // per-link nav transmit status
    QTimer* tickTimer_ = nullptr;
};
