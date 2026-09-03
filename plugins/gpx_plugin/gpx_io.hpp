#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>
#include "route_types.hpp"

// GPX 1.1 reader/writer for the routes/waypoints model. The core's plain-data
// types already map onto GPX (see route_types.hpp), so this is a thin codec:
//   Waypoint   <-> <wpt lat lon> + <time>/<name>/<desc>/<sym>
//   Route      <-> <rte> + <name>/<desc> with ordered <rtept>, plus voyage-plan
//                  extensions: <opencpn:planned_speed> (knots),
//                  <opencpn:planned_departure> (UTC) and, inside
//                  <gpxx:RouteExtension>, <gpxx:DisplayColor>
//   RoutePoint <-> <rtept lat lon> + <name>
//
// No external dependency: built on QXmlStreamReader/Writer (Qt6::Core). Reads are
// namespace-tolerant (matched on local names) so files from OpenCPN, Garmin, etc.
// parse even with a default xmlns or vendor prefixes.
namespace gpx {

// Serialize routes + standalone waypoints to a GPX 1.1 document (UTF-8).
QByteArray write(const QVector<Route>& routes, const QVector<Waypoint>& waypoints);

// Parsed contents of a GPX document. ids are left at -1 (these are not yet
// persisted); the caller inserts them into the store to assign ids.
struct Document {
    QVector<Route>    routes;
    QVector<Waypoint> waypoints;
};

// Parse a GPX document. Returns false and sets `err` on malformed XML; unknown
// elements are skipped, so partial/extended files still import what they can.
bool read(const QByteArray& data, Document& out, QString& err);

}  // namespace gpx
