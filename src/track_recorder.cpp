#include "track_recorder.hpp"
#include "route_store.hpp"
#include "nav_data_store.hpp"
#include "settings.hpp"
#include "geo_nav.hpp"

#include <QTimer>

namespace {
// The sampling cadence. Independent of the user's interval: once the time gate
// has opened we re-test the distance gate every second, so the point lands as
// soon as the boat has actually moved.
constexpr int kTickMs = 1000;
}  // namespace

TrackRecorder::TrackRecorder(RouteStore* routes, const NavDataStore* nav,
                             const Settings* settings, QObject* parent)
    : QObject(parent), routes_(routes), nav_(nav), settings_(settings) {
    timer_ = new QTimer(this);
    timer_->setInterval(kTickMs);
    connect(timer_, &QTimer::timeout, this, &TrackRecorder::tick);
}

bool TrackRecorder::ownshipFix(double& lat, double& lon) const {
    if (!nav_) return false;
    const OwnshipState& s = nav_->ownship();
    if (!s.latitudeDeg.valid() || !s.longitudeDeg.valid()) return false;
    lat = s.latitudeDeg.value;
    lon = s.longitudeDeg.value;
    return true;
}

void TrackRecorder::setRecording(bool on) {
    if (on == isRecording()) return;
    if (!on) { stop(/*discardIfEmpty=*/true); return; }

    if (!routes_ || !routes_->isOpen()) {
        emit recordingChanged(false);   // refuse: let the toggle snap back
        return;
    }
    Track t;
    t.createdUtc = QDateTime::currentDateTimeUtc();
    t.name       = RouteStore::trackNameFor(t.createdUtc);
    t.visible    = true;
    const qint64 id = routes_->addTrack(t);
    if (id < 0) {
        emit recordingChanged(false);
        return;
    }
    trackId_   = id;
    havePoint_ = false;
    timer_->start();
    emit recordingChanged(true);
    emit activeTrackChanged(trackId_);
    tick();   // lay the starting point now if a fix is already available
}

void TrackRecorder::stop(bool discardIfEmpty) {
    if (trackId_ < 0) return;
    timer_->stop();
    const qint64 id = trackId_;
    trackId_ = -1;
    havePoint_ = false;
    lastPointUtc_ = QDateTime();

    // A recording that never got under way (no fix, or the boat never moved) is
    // noise in the Tracks list rather than a record of anything.
    if (discardIfEmpty && routes_) {
        const Track* t = routes_->track(id);
        if (t && t->points.size() < 2) routes_->removeTrack(id);
    }
    emit recordingChanged(false);
    emit activeTrackChanged(-1);
}

void TrackRecorder::tick() {
    if (trackId_ < 0 || !routes_) return;

    double lat = 0.0, lon = 0.0;
    if (!ownshipFix(lat, lon)) return;   // no fix: wait, keep the recording open

    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (havePoint_) {
        const double intervalS = settings_ ? settings_->trackIntervalSeconds() : 60.0;
        const double minDistNm = settings_ ? settings_->trackMinDistanceNm()   : 0.025;
        const double elapsedS  = lastPointUtc_.msecsTo(now) / 1000.0;
        if (elapsedS < intervalS) return;
        if (geonav::distanceNm(lastLat_, lastLon_, lat, lon) < minDistNm) return;
    }

    TrackPoint p;
    p.lat = lat;
    p.lon = lon;
    p.timeUtc = now;
    if (!routes_->appendTrackPoint(trackId_, p)) {
        // The track vanished (deleted from the Tracks tab) or the write failed;
        // there is nothing left to record into. Keep whatever was already saved.
        stop(/*discardIfEmpty=*/false);
        return;
    }
    lastLat_ = lat;
    lastLon_ = lon;
    lastPointUtc_ = now;
    havePoint_ = true;
}
