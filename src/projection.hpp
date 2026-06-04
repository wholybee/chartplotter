#pragma once
#include <cmath>

// Spherical (Web) Mercator projection helpers.
//
// ENC / S-57 vector data is delivered in geographic coordinates (longitude /
// latitude on WGS84).  To draw it on a flat screen and to make adjacent chart
// cells line up ("stitch") we project everything into a single planar
// coordinate system measured in metres.  Mercator is the traditional marine
// projection: rhumb lines are straight and the distortion is acceptable for a
// viewer.
namespace proj {

constexpr double PI = 3.14159265358979323846;
constexpr double kEarthRadius = 6378137.0;          // WGS84 semi-major axis (m)
constexpr double kDeg2Rad = PI / 180.0;
constexpr double kRad2Deg = 180.0 / PI;
constexpr double kMaxLatRad = 85.05112878 * kDeg2Rad; // Mercator clamp

inline double lonToX(double lonDeg) {
    return kEarthRadius * lonDeg * kDeg2Rad;
}

inline double latToY(double latDeg) {
    double lat = latDeg * kDeg2Rad;
    if (lat >  kMaxLatRad) lat =  kMaxLatRad;
    if (lat < -kMaxLatRad) lat = -kMaxLatRad;
    return kEarthRadius * std::log(std::tan(PI / 4.0 + lat / 2.0));
}

inline double xToLon(double x) {
    return (x / kEarthRadius) * kRad2Deg;
}

inline double yToLat(double y) {
    return (2.0 * std::atan(std::exp(y / kEarthRadius)) - PI / 2.0) * kRad2Deg;
}

} // namespace proj
