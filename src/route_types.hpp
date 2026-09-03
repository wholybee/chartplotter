#pragma once
#include <QString>
#include <QDateTime>
#include <QVector>

// Plain data types for the routes/waypoints subsystem. Field names and types are
// chosen to map cleanly onto GPX (deferred), so an exporter can later emit
// <wpt>/<rte>/<rtept> without reshaping the model:
//   Waypoint     -> <wpt lat lon> + <name>/<sym>/<desc>/<time>
//   Route        -> <rte> + <name>/<desc>
//   RoutePoint   -> <rtept lat lon> + <name>
//   Track        -> <trk> + <name>/<desc> (a single <trkseg>)
//   TrackPoint   -> <trkpt lat lon> + <time>
//
// id == -1 marks a record not yet persisted (no SQLite row). createdUtc stores
// the GPX <time> instant in UTC.

struct Waypoint {
    qint64    id = -1;
    QString   name;
    double    lat = 0.0;
    double    lon = 0.0;
    QString   symbol;        // GPX <sym>
    QString   description;   // GPX <desc>
    QDateTime createdUtc;    // GPX <time>
    bool      visible = true;
};

struct RoutePoint {
    double  lat = 0.0;
    double  lon = 0.0;
    QString name;            // GPX <rtept><name>, optional
};

struct Route {
    qint64    id = -1;
    QString   name;
    QString   description;
    QDateTime createdUtc;
    bool      visible = true;
    QVector<RoutePoint> points;   // ordered (seq)

    // Voyage plan + display, all optional and mapped onto GPX extensions:
    //   plannedSpeedKts     -> <opencpn:planned_speed>     (knots; 0 == unset)
    //   plannedDepartureUtc -> <opencpn:planned_departure> (UTC instant)
    //   displayColor        -> <gpxx:DisplayColor>         (Garmin colour name,
    //                          e.g. "Red"; empty == app default)
    // The arrival time is derived (departure + distance / speed), never stored.
    double    plannedSpeedKts = 0.0;
    QDateTime plannedDepartureUtc;
    QString   displayColor;
};

// One recorded fix along a track. Unlike a RoutePoint (a place the boat is meant
// to go) a TrackPoint is a place it has been, so it carries the instant it was
// recorded rather than a name.
struct TrackPoint {
    double    lat = 0.0;
    double    lon = 0.0;
    QDateTime timeUtc;       // GPX <trkpt><time>
};

// A recording of the ownship's movement. Points are only ever appended, in time
// order, by TrackRecorder; createdUtc is when recording started and also seeds
// the default name.
struct Track {
    qint64    id = -1;
    QString   name;
    QString   description;
    QDateTime createdUtc;
    bool      visible = true;
    QVector<TrackPoint> points;   // ordered (seq == time order)
};
