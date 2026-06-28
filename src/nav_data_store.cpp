#include "nav_data_store.hpp"
#include <QTimer>
#include <QDateTime>

NavDataStore::NavDataStore(QObject* parent) : QObject(parent) {
    tickTimer_ = new QTimer(this);
    tickTimer_->setInterval(500);   // 2 Hz is plenty for stale-data UI
    connect(tickTimer_, &QTimer::timeout, this, &NavDataStore::tick);
    tickTimer_->start();
}

void NavDataStore::setStaleSeconds(double s) {
    if (s == staleSeconds_) return;
    staleSeconds_ = s;
    if (recompute()) emit ownshipChanged();
}

void NavDataStore::setInvalidSeconds(double s) {
    if (s == invalidSeconds_) return;
    invalidSeconds_ = s;
    if (recompute()) emit ownshipChanged();
}

void NavDataStore::setSourcePriority(const QStringList& orderedSourceIds) {
    sourcePriority_ = orderedSourceIds;
    // Takes effect as sources next publish / age out; no immediate value change.
}

void NavDataStore::setNavigationData(const NavigationData& d) {
    navigation_ = d;
    emit navigationChanged();
}

void NavDataStore::clearNavigation() {
    if (!navigation_.active) return;   // already inactive; nothing to notify
    navigation_ = NavigationData{};
    emit navigationChanged();
}

void NavDataStore::setNavTransmitting(const QString& sourceId, bool on) {
    if (sourceId.isEmpty()) return;
    auto it = navTransmitting_.find(sourceId);
    if (it != navTransmitting_.end() && it.value() == on) return;   // unchanged
    navTransmitting_[sourceId] = on;
    emit navTransmitStateChanged();
}

// ---- publishing ------------------------------------------------------------

// Priority index of a source (0 = highest). Unknown sources rank below all
// listed ones, so a listed source always wins over an unlisted one.
int NavDataStore::rank(const QString& source) const {
    const int i = sourcePriority_.indexOf(source);
    return i < 0 ? sourcePriority_.size() : i;
}

// Decide whether an update from `source` may overwrite the current value.
bool NavDataStore::accept(const NavValue& current, const QString& source) const {
    if (!current.timestampUtc.isValid()) return true;   // nothing there yet
    const double age = current.timestampUtc.msecsTo(QDateTime::currentDateTimeUtc()) / 1000.0;
    if (age >= invalidSeconds_) return true;            // current invalid -> fall back to anyone
    if (current.source == source) return true;          // same source refreshing itself
    return rank(source) <= rank(current.source);        // else only a >= priority source wins
}

bool NavDataStore::setValue(NavValue& v, double value, const NavValueMeta& meta) {
    if (!accept(v, meta.source)) return false;
    v.value        = value;
    v.source       = meta.source;
    v.timestampUtc = meta.timestampUtc.isValid() ? meta.timestampUtc
                                                 : QDateTime::currentDateTimeUtc();
    v.ageSeconds   = 0.0;
    v.freshness    = NavFreshness::Fresh;
    return true;
}

void NavDataStore::publishOwnshipPosition(double latDeg, double lonDeg,
                                          const NavValueMeta& meta) {
    const bool a = setValue(ownship_.latitudeDeg,  latDeg, meta);
    const bool b = setValue(ownship_.longitudeDeg, lonDeg, meta);
    if (a || b) emit ownshipChanged();
}

void NavDataStore::publishCogSog(double cogDegTrue, double sogKnots,
                                 const NavValueMeta& meta) {
    const bool a = setValue(ownship_.cogDegTrue, cogDegTrue, meta);
    const bool b = setValue(ownship_.sogKnots,   sogKnots,   meta);
    if (a || b) emit ownshipChanged();
}

void NavDataStore::publishHeading(std::optional<double> headingDegTrue,
                                  std::optional<double> headingDegMag,
                                  const NavValueMeta& meta) {
    bool changed = false;
    if (headingDegTrue.has_value())
        changed |= setValue(ownship_.headingDegTrue, *headingDegTrue, meta);
    if (headingDegMag.has_value())
        changed |= setValue(ownship_.headingDegMag, *headingDegMag, meta);
    if (changed) emit ownshipChanged();
}

void NavDataStore::publishVariation(double variationDeg, const NavValueMeta& meta) {
    if (setValue(ownship_.variationDeg, variationDeg, meta)) emit ownshipChanged();
}

void NavDataStore::publishDepth(double depthMeters, const NavValueMeta& meta) {
    if (setValue(ownship_.depthMeters, depthMeters, meta)) emit ownshipChanged();
}

void NavDataStore::publishWaterSpeed(double knots, const NavValueMeta& meta) {
    if (setValue(ownship_.waterSpeedKnots, knots, meta)) emit ownshipChanged();
}

void NavDataStore::publishApparentWind(double speedKnots, double angleDeg,
                                       const NavValueMeta& meta) {
    const bool a = setValue(ownship_.apparentWindSpeedKnots, speedKnots, meta);
    const bool b = setValue(ownship_.apparentWindAngleDeg,   angleDeg,   meta);
    if (a || b) emit ownshipChanged();
}

void NavDataStore::publishTrueWind(double speedKnots, double angleDeg,
                                   const NavValueMeta& meta) {
    const bool a = setValue(ownship_.trueWindSpeedKnots, speedKnots, meta);
    const bool b = setValue(ownship_.trueWindAngleDeg,   angleDeg,   meta);
    if (a || b) emit ownshipChanged();
}

void NavDataStore::publishTrueWindDirection(double directionDeg, double speedKnots,
                                            const NavValueMeta& meta) {
    const bool a = setValue(ownship_.trueWindDirectionDeg, directionDeg, meta);
    const bool b = setValue(ownship_.trueWindSpeedKnots,   speedKnots,   meta);
    if (a || b) emit ownshipChanged();
}

// ---- aging -----------------------------------------------------------------

bool NavDataStore::ageValue(NavValue& v, const QDateTime& now) {
    if (!v.timestampUtc.isValid()) {           // never set
        if (v.freshness != NavFreshness::Invalid) {
            v.freshness = NavFreshness::Invalid;
            return true;
        }
        return false;
    }
    v.ageSeconds = v.timestampUtc.msecsTo(now) / 1000.0;
    NavFreshness f;
    if      (v.ageSeconds < staleSeconds_)   f = NavFreshness::Fresh;
    else if (v.ageSeconds < invalidSeconds_) f = NavFreshness::Stale;
    else                                     f = NavFreshness::Invalid;

    if (f != v.freshness) { v.freshness = f; return true; }
    return false;
}

bool NavDataStore::recompute() {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    NavValue* all[] = {
        &ownship_.latitudeDeg,  &ownship_.longitudeDeg, &ownship_.cogDegTrue,
        &ownship_.sogKnots,     &ownship_.waterSpeedKnots,
        &ownship_.headingDegTrue, &ownship_.headingDegMag, &ownship_.variationDeg,
        &ownship_.depthMeters,
        &ownship_.apparentWindAngleDeg, &ownship_.apparentWindSpeedKnots,
        &ownship_.trueWindAngleDeg,     &ownship_.trueWindSpeedKnots,
        &ownship_.trueWindDirectionDeg,
    };
    bool changed = false;
    for (NavValue* v : all) changed |= ageValue(*v, now);
    return changed;
}

void NavDataStore::tick() {
    if (recompute()) emit ownshipChanged();
}
