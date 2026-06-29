#include "product_adapter.hpp"

#include <utility>

namespace product_adapter {
namespace {

// Symbol-bearing kinds are the only ones loadCellFeatures populates objClass /
// attrs / name for; mirror that here so the round trip reproduces exactly which
// features carry a class code and attributes.
bool symbolBearing(FeatureKind k) {
    return k == FeatureKind::Point || k == FeatureKind::OtherArea ||
           k == FeatureKind::OtherLine;
}

GeometryKind geometryKindFor(FeatureKind k) {
    switch (k) {
        case FeatureKind::DepthArea:
        case FeatureKind::LandArea:
        case FeatureKind::OtherArea:
            return GeometryKind::Area;
        case FeatureKind::DepthContour:
        case FeatureKind::Coastline:
        case FeatureKind::OtherLine:
            return GeometryKind::Line;
        case FeatureKind::Sounding:
        case FeatureKind::Point:
            return GeometryKind::Point;
    }
    return GeometryKind::Point;
}

QString nsCode(const std::string& acronym) {
    if (acronym.empty()) return QString();
    return QString::fromLatin1(kS57Ns) + QString::fromStdString(acronym);
}

std::string stripNs(const QString& code) {
    if (code.isEmpty()) return std::string();
    const QString ns = QString::fromLatin1(kS57Ns);
    const QString bare = code.startsWith(ns) ? code.mid(ns.size()) : code;
    return bare.toStdString();
}

} // namespace

ProductId s57ProductId() {
    return ProductId{QStringLiteral("IHO"), QStringLiteral("S-57"), QString()};
}

ProductFeatureSet fromLegacyFeatures(std::vector<Feature> feats,
                                     const QString& cellId) {
    ProductFeatureSet pfs;
    pfs.cell.cellId  = cellId;
    pfs.cell.product = s57ProductId();

    pfs.features.reserve(feats.size());
    BBox cellBox;
    quint64 nextId = 1;

    for (Feature& f : feats) {
        ChartFeature cf;
        cf.stableId        = nextId++;
        cf.classId.product = pfs.cell.product;
        cf.classId.code    = nsCode(f.objClass);
        cf.scaleMin        = f.scaleMin;
        cf.displayPriorityHint = f.zorder;
        cf.bbox            = f.bbox;

        cf.legacy.kind     = f.kind;
        cf.legacy.zorder   = f.zorder;
        cf.legacy.depth    = f.depth;
        cf.legacy.hasDepth = f.hasDepth;
        cf.legacy.name     = std::move(f.name);

        cf.attrs.reserve(f.attrs.size());
        for (auto& a : f.attrs)
            cf.attrs.push_back(AttributeValue{
                QString::fromLatin1(kS57Ns) + QString::fromStdString(a.first),
                QString::fromStdString(a.second)});

        // Move this feature's rings into the shared pool and record the slice.
        cf.geometry.kind      = geometryKindFor(f.kind);
        cf.geometry.firstRing = static_cast<quint32>(pfs.geometry.rings.size());
        cf.geometry.ringCount = static_cast<quint32>(f.rings.size());
        for (auto& ring : f.rings)
            pfs.geometry.rings.push_back(std::move(ring));

        cellBox.expand(f.bbox);
        pfs.features.push_back(std::move(cf));
    }

    pfs.cell.bbox = cellBox;
    return pfs;
}

std::vector<Feature> toLegacyFeatures(ProductFeatureSet pfs, BBox& bbox) {
    std::vector<Feature> out;
    out.reserve(pfs.features.size());

    for (ChartFeature& cf : pfs.features) {
        Feature f;
        f.kind     = cf.legacy.kind;
        f.zorder   = cf.legacy.zorder;
        f.depth    = cf.legacy.depth;
        f.hasDepth = cf.legacy.hasDepth;
        f.scaleMin = cf.scaleMin;
        f.bbox     = cf.bbox;
        f.name     = std::move(cf.legacy.name);

        // objClass and attrs were only ever populated for symbol-bearing kinds.
        if (symbolBearing(f.kind)) {
            f.objClass = stripNs(cf.classId.code);
            f.attrs.reserve(cf.attrs.size());
            for (const AttributeValue& a : cf.attrs)
                f.attrs.emplace_back(stripNs(a.id), a.value.toString().toStdString());
        }

        // Move the feature's rings back out of the shared pool.
        const quint32 first = cf.geometry.firstRing;
        const quint32 count = cf.geometry.ringCount;
        f.rings.reserve(count);
        for (quint32 i = 0; i < count; ++i)
            f.rings.push_back(std::move(pfs.geometry.rings[first + i]));

        out.push_back(std::move(f));
    }

    bbox = pfs.cell.bbox;
    return out;
}

} // namespace product_adapter
