#pragma once
#include <QObject>
#include <QDateTime>

class RouteStore;
class NavDataStore;
class Settings;
class QTimer;

// Records the ownship's movement into a Track in the RouteStore.
//
// While recording, a 1 s tick samples the ownship fix and lays down a track point
// once BOTH gates open: the configured interval has elapsed since the last point,
// and the boat has moved at least the configured distance from it. Once the time
// gate is open the tick keeps checking every second, so a point is laid the moment
// the boat has moved far enough rather than waiting for the next whole interval.
// A boat sitting at anchor therefore records nothing beyond its first point.
//
// Points are appended to SQLite one at a time as they are taken, so a power loss
// costs at most the current interval. Stopping a recording that never gathered a
// second point discards the track — a single point is not a track.
class TrackRecorder : public QObject {
    Q_OBJECT
public:
    TrackRecorder(RouteStore* routes, const NavDataStore* nav, const Settings* settings,
                  QObject* parent = nullptr);

    bool   isRecording()   const { return trackId_ >= 0; }
    qint64 activeTrackId() const { return trackId_; }   // -1 when idle

public slots:
    void setRecording(bool on);

signals:
    // Emitted whenever recording starts or stops, including when a start is
    // refused (store unavailable) — so a toggle button can always mirror truth.
    void recordingChanged(bool on);
    // The track being recorded into, or -1 when idle. The chart overlay draws it
    // brighter than the finished ones.
    void activeTrackChanged(qint64 id);

private slots:
    void tick();

private:
    bool ownshipFix(double& lat, double& lon) const;   // false when no valid fix
    void stop(bool discardIfEmpty);

    RouteStore*         routes_ = nullptr;
    const NavDataStore* nav_ = nullptr;
    const Settings*     settings_ = nullptr;
    QTimer*             timer_ = nullptr;

    qint64    trackId_ = -1;      // >= 0 while recording
    bool      havePoint_ = false; // a first point has been laid
    double    lastLat_ = 0.0;
    double    lastLon_ = 0.0;
    QDateTime lastPointUtc_;
};
