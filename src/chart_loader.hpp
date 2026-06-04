#pragma once
#include <string>
#include <vector>

// A projected point, in Mercator metres.
struct Pt {
    double x;
    double y;
};

// Axis-aligned bounding box in projected coordinates. Used both for the whole
// chart extent and for per-feature culling.
struct BBox {
    double minx =  1e30, miny =  1e30;
    double maxx = -1e30, maxy = -1e30;

    void expand(double x, double y) {
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    void expand(const BBox& b) {
        if (!b.valid()) return;
        expand(b.minx, b.miny);
        expand(b.maxx, b.maxy);
    }
    bool valid() const { return maxx >= minx && maxy >= miny; }
    bool intersects(const BBox& o) const {
        return !(o.minx > maxx || o.maxx < minx || o.miny > maxy || o.maxy < miny);
    }
};

// Simplified classification of S-57 object classes into things we know how to
// draw. Everything else collapses into the generic Area/Line/Point buckets.
enum class FeatureKind {
    DepthArea,     // DEPARE / DRGARE  - water, shaded by depth
    LandArea,      // LNDARE           - land fill
    OtherArea,     // generic polygon  - drawn as outline only
    DepthContour,  // DEPCNT
    Coastline,     // COALNE / SLCONS
    OtherLine,     // generic line
    Sounding,      // SOUNDG           - point with a depth value
    Point          // generic point (buoys, beacons, lights, ...)
};

struct Feature {
    FeatureKind kind = FeatureKind::Point;
    int zorder = 0;
    // For polygons: ring[0] is the outer ring, the rest are holes.
    // For lines: each entry is one path.
    // For points: a single ring with a single point.
    std::vector<std::vector<Pt>> rings;
    double depth = 0.0;   // metres, valid only when hasDepth is true
    bool hasDepth = false;
    BBox bbox;
};

// Loads every ENC base cell (*.000) found under a directory, projects all
// geometry into a common Mercator space, and exposes the merged result.
class ChartSet {
public:
    // Returns true on success. On failure, errorOut is filled in.
    bool loadDirectory(const std::string& dir, std::string& errorOut);

    const std::vector<Feature>& features() const { return features_; }
    const BBox& bounds() const { return bounds_; }
    std::size_t cellCount() const { return cellCount_; }
    void clear();

private:
    bool loadCell(const std::string& path);

    std::vector<Feature> features_;
    BBox bounds_;
    std::size_t cellCount_ = 0;
};
