#include "chart_loader.hpp"
#include "projection.hpp"

#include <gdal.h>
#include <ogr_api.h>
#include <ogr_core.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// Symbology-relevant attribute acronyms (set once via chart::setSymbologyAttrs
// before any worker load). Read-only during loads, so no synchronization is
// needed. Populated from the symbol atlas's lookup-condition attribute set.
std::vector<std::string> g_symAttrs;

// Read an OGR field as a normalized S-57 value string: a plain integer ("4"),
// or a comma-joined list for multi-valued attributes like COLOUR ("1,4"), with
// no spaces.  Matches the value encoding baked into symbols.bin by
// gen_symbols.
//
// Multi-valued S-57 attributes (COLOUR, COLPAT, NATSUR, etc.) can be reported
// by GDAL's S-57 driver as OFTStringList or OFTIntegerList depending on the
// driver build and version.  StringList is the modern default — without the
// case here, OGR_F_GetFieldAsString would return a list-formatted string like
// "(1:4)" instead of the bare value "4", breaking every COLOUR-conditioned
// lookup (lights silently became magenta, lateral buoys became uncoloured
// defaults).
std::string normalizedFieldValue(OGRFeatureH feat, int idx) {
    const OGRFieldType t = OGR_Fld_GetType(OGR_F_GetFieldDefnRef(feat, idx));
    switch (t) {
        case OFTInteger:
            return std::to_string(OGR_F_GetFieldAsInteger(feat, idx));
        case OFTInteger64:
            return std::to_string(OGR_F_GetFieldAsInteger64(feat, idx));
        case OFTReal: {
            // Integer-valued reals print without decimals (keeps enumerated
            // codes clean for exact-string lookup matching); fractional values
            // keep their decimals so TX()/TE() labels show e.g. a 12.5 m
            // vertical clearance rather than a truncated "12". No lookup
            // condition tests a real-valued attribute, so this can't affect
            // best-match selection.
            const double v = OGR_F_GetFieldAsDouble(feat, idx);
            if (v == std::floor(v) && std::fabs(v) < 1e15)
                return std::to_string(static_cast<long long>(v));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3f", v);
            std::string s(buf);
            const auto dot = s.find('.');
            if (dot != std::string::npos) {
                const auto last = s.find_last_not_of('0');
                if (last != std::string::npos) s.erase(last + 1);
                if (!s.empty() && s.back() == '.') s.pop_back();
            }
            return s;
        }
        case OFTIntegerList: {
            int n = 0;
            const int* v = OGR_F_GetFieldAsIntegerList(feat, idx, &n);
            std::string s;
            for (int i = 0; i < n; ++i) {
                if (i) s += ',';
                s += std::to_string(v[i]);
            }
            return s;
        }
        case OFTRealList: {
            int n = 0;
            const double* v = OGR_F_GetFieldAsDoubleList(feat, idx, &n);
            std::string s;
            for (int i = 0; i < n; ++i) {
                if (i) s += ',';
                s += std::to_string(static_cast<long long>(v[i]));
            }
            return s;
        }
        case OFTStringList: {
            char** v = OGR_F_GetFieldAsStringList(feat, idx);
            std::string s;
            if (v) {
                for (int i = 0; v[i]; ++i) {
                    if (i) s += ',';
                    for (const char* p = v[i]; *p; ++p)
                        if (*p != ' ') s += *p;
                }
            }
            return s;
        }
        default: {
            // OFTString and anything else. Enumerated / list attributes arrive
            // as a bare code or comma list ("4", "1, 4"), sometimes with stray
            // spaces that must go for symbology lookups to match. But free-text
            // attributes (INFORM, NINFOM, ...) carry real sentences whose spaces
            // must be preserved, or they display as one run-on word. Tell them
            // apart by content: strip every space only when the value is purely
            // a numeric code/list; otherwise keep internal spacing and trim ends.
            const char* s = OGR_F_GetFieldAsString(feat, idx);
            if (!s) return std::string();
            const std::string raw = s;
            bool codeLike = true;
            for (unsigned char c : raw)
                if (!std::isdigit(c) && c != ',' && c != ' ') { codeLike = false; break; }
            if (codeLike) {
                std::string out;
                for (char c : raw)
                    if (c != ' ') out += c;
                return out;
            }
            const auto b = raw.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) return std::string();
            const auto e = raw.find_last_not_of(" \t\r\n");
            return raw.substr(b, e - b + 1);
        }
    }
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

bool skipLayer(const std::string& name) {
    if (name.size() >= 2 && name[0] == 'M' && name[1] == '_') return true; // M_COVR, M_QUAL...
    if (name == "DSID") return true;
    return false;
}

void projectSimple(OGRGeometryH g, std::vector<Pt>& out, BBox& bbox) {
    int n = OGR_G_GetPointCount(g);
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        double x = proj::lonToX(OGR_G_GetX(g, i));
        double y = proj::latToY(OGR_G_GetY(g, i));
        out.push_back({x, y});
        bbox.expand(x, y);
    }
}

// Load every polygon in a shapefile as features of one kind/zorder. Returns
// false only if the file can't be opened (e.g. an absent optional level).
bool loadPolygonShp(const std::string& path, FeatureKind kind, int zorder,
                    std::vector<Feature>& out);

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

bool loadPolygonShp(const std::string& path, FeatureKind kind, int zorder,
                    std::vector<Feature>& out) {
    GDALDatasetH ds = GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
    if (!ds) return false;

    int layerCount = GDALDatasetGetLayerCount(ds);
    for (int li = 0; li < layerCount; ++li) {
        OGRLayerH layer = GDALDatasetGetLayer(ds, li);
        if (!layer) continue;
        OGR_L_ResetReading(layer);
        OGRFeatureH feat;
        while ((feat = OGR_L_GetNextFeature(layer)) != nullptr) {
            OGRGeometryH geom = OGR_F_GetGeometryRef(feat);
            if (geom) {
                Feature f;
                f.kind = kind;
                f.zorder = zorder;
                extractGeometry(geom, f);
                if (!f.rings.empty()) out.push_back(std::move(f));
            }
            OGR_F_Destroy(feat);
        }
    }
    GDALClose(ds);
    return true;
}

// Collect the exterior ring of each polygon under `g` (recursing through multi-
// polygons / collections), projected to Mercator, into `rings`. Some producers
// emit M_COVR coverage as bare rings/linestrings; those are taken whole.
void collectExteriorRings(OGRGeometryH g, std::vector<std::vector<Pt>>& rings,
                          BBox& bbox) {
    if (!g) return;
    const OGRwkbGeometryType t = wkbFlatten(OGR_G_GetGeometryType(g));
    switch (t) {
        case wkbPolygon: {
            if (OGR_G_GetGeometryCount(g) > 0) {
                std::vector<Pt> pts;
                projectSimple(OGR_G_GetGeometryRef(g, 0), pts, bbox);  // ring 0 = exterior
                if (pts.size() >= 3) rings.push_back(std::move(pts));
            }
            break;
        }
        case wkbMultiPolygon:
        case wkbGeometryCollection: {
            const int n = OGR_G_GetGeometryCount(g);
            for (int i = 0; i < n; ++i)
                collectExteriorRings(OGR_G_GetGeometryRef(g, i), rings, bbox);
            break;
        }
        case wkbLineString:
        case wkbLinearRing: {
            std::vector<Pt> pts;
            projectSimple(g, pts, bbox);
            if (pts.size() >= 3) rings.push_back(std::move(pts));
            break;
        }
        default:
            break;
    }
}

} // namespace

namespace chart {

void init(const std::string& gdalDataDir) {
    // Point GDAL at the bundled data directory before registering drivers.
    // This resolves S-57 object-class codes (OBJL) → named strings so that
    // the layer classifier (classify()) can match "DEPARE", "LNDARE" etc.
    // Without this, a machine without GDAL installed system-wide renders charts
    // as grey outlines only — geometry loads but all fill/colour is lost.
    if (!gdalDataDir.empty())
        CPLSetConfigOption("GDAL_DATA", gdalDataDir.c_str());

    CPLSetConfigOption("OGR_S57_OPTIONS",
        "SPLIT_MULTIPOINT=ON,ADD_SOUNDG_DEPTH=ON,RETURN_PRIMITIVES=OFF,"
        "RETURN_LINKAGES=OFF,LNAM_REFS=OFF");
    GDALAllRegister();
}

void setSymbologyAttrs(const std::vector<std::string>& acronyms) {
    g_symAttrs = acronyms;
}

bool loadCellFeatures(const std::string& path,
                      std::vector<Feature>& out, BBox& bbox, std::string& err) {
    GDALDatasetH ds = GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
    if (!ds) { err = "GDAL could not open: " + path; return false; }

    int layerCount = GDALDatasetGetLayerCount(ds);
    for (int li = 0; li < layerCount; ++li) {
        OGRLayerH layer = GDALDatasetGetLayer(ds, li);
        if (!layer) continue;
        const char* rawName = OGR_L_GetName(layer);
        std::string layerName = rawName ? rawName : "";
        if (skipLayer(layerName)) continue;

        // Resolve, once per layer, the field index of each symbology-relevant
        // attribute that this layer's schema actually carries. Per-feature
        // reads then index directly instead of searching by name each time.
        std::vector<std::pair<std::string, int>> attrFields;
        OGRFeatureDefnH layerDefn = OGR_L_GetLayerDefn(layer);
        if (!g_symAttrs.empty()) {
            for (const std::string& a : g_symAttrs) {
                int fi = OGR_FD_GetFieldIndex(layerDefn, a.c_str());
                if (fi >= 0) attrFields.emplace_back(a, fi);
            }
        }
        // SCAMIN governs at what zoom point objects declutter; resolve its field
        // index once per layer (it carries on most S-57 layers).
        const int scaminIdx = layerDefn ? OGR_FD_GetFieldIndex(layerDefn, "SCAMIN") : -1;
        // OBJNAM is the object's name, drawn as a text label. Resolved once per
        // layer like SCAMIN.
        const int objnamIdx = layerDefn ? OGR_FD_GetFieldIndex(layerDefn, "OBJNAM") : -1;

        OGR_L_ResetReading(layer);
        OGRFeatureH feat;
        while ((feat = OGR_L_GetNextFeature(layer)) != nullptr) {
            OGRGeometryH geom = OGR_F_GetGeometryRef(feat);
            if (geom) {
                OGRwkbGeometryType gt = wkbFlatten(OGR_G_GetGeometryType(geom));
                Feature f;
                f.kind = classify(layerName, gt);
                f.zorder = zorderFor(f.kind);
                // Symbol-bearing and styled-line kinds carry their object
                // class + attributes so the build step can resolve them via
                // the S-52 lookup engine: points/areas resolve to a symbol,
                // generic lines (TSSBND, restricted-area boundaries, etc.)
                // resolve to an LS() pen style, and areas can resolve to both
                // (centred symbol plus boundary style).
                const bool symbolBearing = (f.kind == FeatureKind::Point ||
                                            f.kind == FeatureKind::OtherArea ||
                                            f.kind == FeatureKind::OtherLine);
                if (symbolBearing) {
                    f.objClass = layerName;
                    for (const auto& af : attrFields) {
                        if (OGR_F_IsFieldSetAndNotNull(feat, af.second))
                            f.attrs.emplace_back(af.first,
                                                 normalizedFieldValue(feat, af.second));
                    }
                    if (objnamIdx >= 0 && OGR_F_IsFieldSetAndNotNull(feat, objnamIdx)) {
                        const char* nm = OGR_F_GetFieldAsString(feat, objnamIdx);
                        if (nm) f.name = nm;
                    }
                }
                if (scaminIdx >= 0 && OGR_F_IsFieldSetAndNotNull(feat, scaminIdx))
                    f.scaleMin = OGR_F_GetFieldAsInteger(feat, scaminIdx);
                extractGeometry(geom, f);
                if (!f.rings.empty()) {
                    if (f.kind == FeatureKind::DepthArea) {
                        int idx = OGR_F_GetFieldIndex(feat, "DRVAL1");
                        if (idx >= 0 && OGR_F_IsFieldSetAndNotNull(feat, idx)) {
                            f.depth = OGR_F_GetFieldAsDouble(feat, idx);
                            f.hasDepth = true;
                        }
                    } else if (f.kind == FeatureKind::Sounding) {
                        int idx = OGR_F_GetFieldIndex(feat, "DEPTH");
                        if (idx >= 0 && OGR_F_IsFieldSetAndNotNull(feat, idx)) {
                            f.depth = OGR_F_GetFieldAsDouble(feat, idx);
                            f.hasDepth = true;
                        }
                    }
                    bbox.expand(f.bbox);
                    out.push_back(std::move(f));
                }
            }
            OGR_F_Destroy(feat);
        }
    }
    GDALClose(ds);

    // Painter's order within the cell: areas, then lines, then points.
    std::stable_sort(out.begin(), out.end(),
                     [](const Feature& a, const Feature& b) { return a.zorder < b.zorder; });
    return true;
}

bool computeCellExtentLonLat(const std::string& path,
                             double& minLon, double& minLat,
                             double& maxLon, double& maxLat, std::string& err) {
    GDALDatasetH ds = GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
    if (!ds) { err = "GDAL could not open: " + path; return false; }

    OGREnvelope total;
    total.MinX = total.MinY =  1e30;
    total.MaxX = total.MaxY = -1e30;
    bool have = false;

    auto absorb = [&](OGRLayerH layer) {
        OGREnvelope env;
        if (layer && OGR_L_GetExtent(layer, &env, TRUE) == OGRERR_NONE) {
            if (env.MinX < total.MinX) total.MinX = env.MinX;
            if (env.MinY < total.MinY) total.MinY = env.MinY;
            if (env.MaxX > total.MaxX) total.MaxX = env.MaxX;
            if (env.MaxY > total.MaxY) total.MaxY = env.MaxY;
            have = true;
        }
    };

    // Prefer the small coverage layer; it is the true cell footprint.
    OGRLayerH cov = GDALDatasetGetLayerByName(ds, "M_COVR");
    if (cov) {
        absorb(cov);
    }
    if (!have) {
        int n = GDALDatasetGetLayerCount(ds);
        for (int i = 0; i < n; ++i)
            absorb(GDALDatasetGetLayer(ds, i));
    }
    GDALClose(ds);

    if (!have) { err = "No extent found in: " + path; return false; }
    minLon = total.MinX; minLat = total.MinY;
    maxLon = total.MaxX; maxLat = total.MaxY;
    return true;
}

bool computeCellCoverage(const std::string& path,
                         std::vector<std::vector<Pt>>& rings, BBox& bbox,
                         std::string& err) {
    rings.clear();
    bbox = BBox{};
    GDALDatasetH ds = GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                                 nullptr, nullptr, nullptr);
    if (!ds) { err = "GDAL could not open: " + path; return false; }

    OGRLayerH cov = GDALDatasetGetLayerByName(ds, "M_COVR");
    if (!cov) { GDALClose(ds); return false; }   // no coverage layer: caller falls back

    // CATCOV (1 = coverage available, 2 = none). Filter to 1 when the field is
    // present; if a producer omits it, take every polygon.
    OGRFeatureDefnH defn = OGR_L_GetLayerDefn(cov);
    const int catIdx = defn ? OGR_FD_GetFieldIndex(defn, "CATCOV") : -1;

    OGR_L_ResetReading(cov);
    OGRFeatureH feat;
    while ((feat = OGR_L_GetNextFeature(cov)) != nullptr) {
        bool take = true;
        if (catIdx >= 0 && OGR_F_IsFieldSetAndNotNull(feat, catIdx))
            take = (OGR_F_GetFieldAsInteger(feat, catIdx) == 1);
        if (take)
            collectExteriorRings(OGR_F_GetGeometryRef(feat), rings, bbox);
        OGR_F_Destroy(feat);
    }
    GDALClose(ds);

    if (rings.empty() || !bbox.valid()) { err = "No M_COVR coverage in: " + path; return false; }
    return true;
}

bool loadBasemap(const std::string& gshhgRoot, const std::string& tier,
                 std::vector<Feature>& out, std::string& err) {
    const std::string base =
        gshhgRoot + "/GSHHS_shp/" + tier + "/GSHHS_" + tier + "_";
    // L1 = land (required). zorder 0 so it sits beneath the lakes.
    if (!loadPolygonShp(base + "L1.shp", FeatureKind::LandArea, 0, out)) {
        err = "Could not open basemap shoreline: " + base + "L1.shp";
        return false;
    }
    // L2 = lakes, drawn as water over the land. L3/L4 (islands-in-lakes, ponds)
    // and L5/L6 (Antarctica) can be layered in later. Optional — ignore if absent.
    loadPolygonShp(base + "L2.shp", FeatureKind::DepthArea, 1, out);
    return !out.empty();
}

} // namespace chart
