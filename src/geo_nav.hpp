#pragma once
#include <cmath>
#include <algorithm>

// Spherical great-circle geodesy for route navigation, in nautical miles and
// degrees. Separate from proj:: (which is Web-Mercator for drawing) because
// navigation needs true distances and bearings on the sphere, not planar metres.
//
// Formulas follow the standard great-circle set (haversine distance, initial
// bearing, cross-track / along-track distance). Accuracy is well within a metre
// over the leg lengths a small craft routes, and bearings are exact at the
// present position.
namespace geonav {

constexpr double PI  = 3.14159265358979323846;
constexpr double D2R = PI / 180.0;
constexpr double R2D = 180.0 / PI;
// Mean Earth radius (6 371 000 m) expressed in nautical miles (1 nm = 1852 m).
constexpr double kEarthRadiusNm = 6371000.0 / 1852.0;   // ~3440.07 nm

// Wrap an angle to [0, 360).
inline double norm360(double deg) {
    deg = std::fmod(deg, 360.0);
    if (deg < 0.0) deg += 360.0;
    return deg;
}

// Great-circle distance between two lat/lon points, in nautical miles.
inline double distanceNm(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * D2R, p2 = lat2 * D2R;
    const double dphi = (lat2 - lat1) * D2R;
    const double dl   = (lon2 - lon1) * D2R;
    double a = std::sin(dphi / 2) * std::sin(dphi / 2)
             + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    // Clamp to [0, 1]. For near-antipodal points (a route on the far side of the
    // globe from the boat) floating-point rounding can push `a` a hair past 1,
    // making sqrt(1 - a) the square root of a negative number — i.e. NaN. That
    // NaN then poisons every value derived from this distance (range, cross-track
    // error), and crashes the navigation display the moment it tries to paint a
    // marker at a NaN coordinate.
    a = std::clamp(a, 0.0, 1.0);
    const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
    return kEarthRadiusNm * c;
}

// Initial (forward) great-circle bearing from point 1 to point 2, degrees true
// in [0, 360).
inline double initialBearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * D2R, p2 = lat2 * D2R;
    const double dl = (lon2 - lon1) * D2R;
    const double y = std::sin(dl) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2)
                   - std::sin(p1) * std::cos(p2) * std::cos(dl);
    return norm360(std::atan2(y, x) * R2D);
}

// Signed cross-track distance (nm) of point P relative to the great-circle path
// from origin (1) to destination (2). Positive => P is to the RIGHT of the track
// (so steer left to regain it); negative => to the LEFT (steer right).
inline double crossTrackNm(double latP, double lonP,
                           double lat1, double lon1, double lat2, double lon2) {
    const double d13  = distanceNm(lat1, lon1, latP, lonP) / kEarthRadiusNm;  // angular
    const double th13 = initialBearingDeg(lat1, lon1, latP, lonP) * D2R;
    const double th12 = initialBearingDeg(lat1, lon1, lat2, lon2) * D2R;
    const double xt = std::asin(std::clamp(std::sin(d13) * std::sin(th13 - th12),
                                           -1.0, 1.0));
    return xt * kEarthRadiusNm;
}

// Signed along-track distance (nm) from origin (1) to the foot of the
// perpendicular dropped from P onto the origin->destination (2) path. Positive in
// the direction of the leg; NEGATIVE when P projects behind the origin (the boat
// is on the far side of the origin from the destination). Compared against the
// leg length, this tells whether the perpendicular at the destination has been
// passed — the sign is essential, so a boat that is merely short of the leg is
// never mistaken for one beyond its end.
inline double alongTrackNm(double latP, double lonP,
                           double lat1, double lon1, double lat2, double lon2) {
    const double d13 = distanceNm(lat1, lon1, latP, lonP) / kEarthRadiusNm;
    const double xt  = crossTrackNm(latP, lonP, lat1, lon1, lat2, lon2) / kEarthRadiusNm;
    const double ratio = std::clamp(std::cos(d13) / std::cos(xt), -1.0, 1.0);
    double dat = std::acos(ratio);   // magnitude of the projection from the origin
    // Sign by direction: if the bearing origin->P differs from the bearing
    // origin->destination by more than 90 degrees, P is behind the origin.
    const double th13 = initialBearingDeg(lat1, lon1, latP, lonP) * D2R;
    const double th12 = initialBearingDeg(lat1, lon1, lat2, lon2) * D2R;
    if (std::cos(th13 - th12) < 0.0) dat = -dat;
    return dat * kEarthRadiusNm;
}

// Convert a true bearing to magnetic given the local variation in degrees
// (easterly positive): magnetic = true - variation.
inline double magneticFromTrue(double trueDeg, double variationDeg) {
    return norm360(trueDeg - variationDeg);
}

}  // namespace geonav
