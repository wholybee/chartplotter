#include "chart_loader.hpp"
#include "projection.hpp"

#include <gdal.h>
#include <ogr_api.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

// --- helpers ---------------------------------------------------------------

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

int zorderFor(FeatureKind k) {
    switch (k) {
        case FeatureKind::DepthArea:    return 0;
        case FeatureKind::OtherArea:    return 5;
        case FeatureKind::LandArea:     return 10;
        case FeatureKind::OtherLine:    return 20;
        case FeatureKind::DepthContour: return 21;
        case FeatureKind::Coastline:    return 22;
        case FeatureKind::Sounding:     return 30;
        case FeatureKind::Point:        return 40;
    }
    return 50;
}

FeatureKind classify(const std::string& n, OGRwkbGeometryType t) {
    if (n == "DEPARE" || n == "DRGARE") return FeatureKind::DepthArea;
    if (n == "LNDARE")                  return FeatureKind::LandArea;
    if (n == "COALNE" || n == "SLCONS") return FeatureKind::Coastline;
    if (n == "DEPCNT")                  return FeatureKind::DepthContour;
    if (n == "SOUNDG")                  return FeatureKind::Sounding;

    switch (t) {
        case wkbPolygon:
        case wkbMultiPolygon:    return FeatureKind::OtherArea;
        case wkbLineString:
        case wkbMultiLineString:
        case wkbLinearRing:      return FeatureKind::OtherLine;
        default:                 return FeatureKind::Point;
    }
}

// Meta / coverage layers we never want to draw (they would paint big opaque
// rectangles over the whole cell).
bool skipLayer(const std::string& name) {
    if (name.size() >= 2 && name[0] == 'M' && name[1] == '_') return true; // M_COVR, M_QUAL...
    if (name == "DSID") return true;                                       // dataset id, no geom
    return false;
}

// Project one simple geometry (point / line / ring) into a vector of points.
void projectSimple(OGRGeometryH g, std::vector<Pt>& out, BBox& bbox) {
    int n = OGR_G_GetPointCount(g);
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        double lon = OGR_G_GetX(g, i);
        double lat = OGR_G_GetY(g, i);
        double x = proj::lonToX(lon);
        double y = proj::latToY(lat);
        out.push_back({x, y});
        bbox.expand(x, y);
    }
}

// Recursively flatten any geometry into the feature's list of rings.
void extractGeometry(OGRGeometryH g, Feature& f) {
    if (!g) return;
    OGRwkbGeometryType t = wkbFlatten(OGR_G_GetGeometryType(g));
    switch (t) {
        case wkbPoint:
        case wkbLineString:
        case wkbLinearRing: {
            std::vector<Pt> pts;
            projectSimple(g, pts, f.bbox);
            if (!pts.empty()) f.rings.push_back(std::move(pts));
            break;
        }
        case wkbPolygon:
        case wkbMultiPoint:
        case wkbMultiLineString:
        case wkbMultiPolygon:
        case wkbGeometryCollection: {
            int count = OGR_G_GetGeometryCount(g);
            for (int i = 0; i < count; ++i)
                extractGeometry(OGR_G_GetGeometryRef(g, i), f);
            break;
        }
        default:
            break;
    }
}

} // namespace

// --- ChartSet --------------------------------------------------------------

void ChartSet::clear() {
    features_.clear();
    bounds_ = BBox{};
    cellCount_ = 0;
}

bool ChartSet::loadCell(const std::string& path) {
    GDALDatasetH ds = GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
    if (!ds) return false;

    int layerCount = GDALDatasetGetLayerCount(ds);
    for (int li = 0; li < layerCount; ++li) {
        OGRLayerH layer = GDALDatasetGetLayer(ds, li);
        if (!layer) continue;

        const char* rawName = OGR_L_GetName(layer);
        std::string layerName = rawName ? rawName : "";
        if (skipLayer(layerName)) continue;

        OGR_L_ResetReading(layer);
        OGRFeatureH feat;
        while ((feat = OGR_L_GetNextFeature(layer)) != nullptr) {
            OGRGeometryH geom = OGR_F_GetGeometryRef(feat);
            if (geom) {
                OGRwkbGeometryType gt = wkbFlatten(OGR_G_GetGeometryType(geom));

                Feature f;
                f.kind = classify(layerName, gt);
                f.zorder = zorderFor(f.kind);
                extractGeometry(geom, f);

                if (!f.rings.empty()) {
                    if (f.kind == FeatureKind::DepthArea) {
                        // DRVAL1 is the shoalest depth of the range -> drives shading.
                        int idx = OGR_F_GetFieldIndex(feat, "DRVAL1");
                        if (idx >= 0 && OGR_F_IsFieldSetAndNotNull(feat, idx)) {
                            f.depth = OGR_F_GetFieldAsDouble(feat, idx);
                            f.hasDepth = true;
                        }
                    } else if (f.kind == FeatureKind::Sounding) {
                        // Added by the S57 driver when ADD_SOUNDG_DEPTH=ON.
                        int idx = OGR_F_GetFieldIndex(feat, "DEPTH");
                        if (idx >= 0 && OGR_F_IsFieldSetAndNotNull(feat, idx)) {
                            f.depth = OGR_F_GetFieldAsDouble(feat, idx);
                            f.hasDepth = true;
                        }
                    }
                    bounds_.expand(f.bbox);
                    features_.push_back(std::move(f));
                }
            }
            OGR_F_Destroy(feat);
        }
    }

    GDALClose(ds);
    ++cellCount_;
    return true;
}

bool ChartSet::loadDirectory(const std::string& dir, std::string& errorOut) {
    clear();

    // S-57 driver tuning: split multipoint soundings into individual points and
    // attach a DEPTH attribute so we can label them.
    CPLSetConfigOption("OGR_S57_OPTIONS",
        "SPLIT_MULTIPOINT=ON,ADD_SOUNDG_DEPTH=ON,RETURN_PRIMITIVES=OFF,"
        "RETURN_LINKAGES=OFF,LNAM_REFS=OFF");
    GDALAllRegister();

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        errorOut = "Not a directory:\n" + dir;
        return false;
    }

    // Recursively collect every ENC base cell (*.000).
    std::vector<std::string> cells;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(
                 dir, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            if (toLower(entry.path().extension().string()) == ".000")
                cells.push_back(entry.path().string());
        }
    } catch (const std::exception& e) {
        if (cells.empty()) {
            errorOut = std::string("Could not scan directory:\n") + e.what();
            return false;
        }
        // Otherwise: partial scan, keep what we found.
    }

    if (cells.empty()) {
        errorOut = "No ENC cells (*.000) were found under:\n" + dir;
        return false;
    }

    std::sort(cells.begin(), cells.end());
    for (const auto& c : cells)
        loadCell(c);

    if (cellCount_ == 0) {
        errorOut = "Found cell files but none could be opened by GDAL.";
        return false;
    }
    if (!bounds_.valid()) {
        errorOut = "The charts loaded but contained no drawable geometry.";
        return false;
    }

    // Painter's algorithm: areas, then lines, then point symbols.
    std::stable_sort(features_.begin(), features_.end(),
                     [](const Feature& a, const Feature& b) {
                         return a.zorder < b.zorder;
                     });
    return true;
}
