#pragma once
#include <QString>
#include <QVariant>
#include <cstdint>
#include <string>
#include <vector>
#include "chart_loader.hpp"   // Pt, BBox, FeatureKind

// ============================================================================
// Stage 3: normalized chart-product feature model (renderer architecture plan,
// Layer 2 — docs/renderer_architecture_plan.md).
// ============================================================================
//
// The legacy `Feature` (chart_loader.hpp) bakes S-57 assumptions into the one
// internal type: a 6-char object-class acronym, ad-hoc string attributes, and a
// render-oriented `FeatureKind`. That is the wrong universal model for S-101 and
// future S-1xx products, whose feature/attribute identities live in their own
// catalogues.
//
// This model is product-neutral. Identity is a namespace-qualified
// `FeatureClassId` carrying a `ProductId`; attributes are typed and
// namespace-qualified; geometry lives in a cell-local pool referenced by slice
// so rings are not copied per feature. S-57 is just one product that decodes
// into it (see product_decoder / product_adapter), and the existing renderer
// keeps working through a compatibility adapter back to `Feature`.
//
// TRANSITIONAL: ChartFeature still carries `LegacyRenderHints` because the
// current S-52/QPainter build step consumes `Feature`'s kind/zorder/depth. Once
// portrayal (Stage 4) derives those from FeatureClassId + attributes, the hints
// and the back-adapter go away.

// Which product/spec a cell or feature came from. Never an enum: products are
// open-ended (IHO S-57/S-101/S-102…, C-MAP CM93, app-private), so they are
// identified by string triplet.
struct ProductId {
    QString authority;   // "IHO", "CMAP", app-private, …
    QString product;     // "S-57", "S-101", "CM93", "S-102"
    QString edition;     // product-spec edition, if known

    bool operator==(const ProductId& o) const {
        return authority == o.authority && product == o.product &&
               edition == o.edition;
    }
};

// Namespace-qualified feature class. `code` is the catalogue identity prefixed
// by a short namespace tag, e.g. "S57:DEPARE" or "S101:DepthArea" — deliberately
// NOT a bare S-57 acronym, so portrayal packages can target a product cleanly.
struct FeatureClassId {
    ProductId product;
    QString   code;
};

// A typed, namespace-qualified attribute. `value` holds the natural type
// (int/double/string/list/date) rather than forcing everything to a string as
// the legacy path did. For the S-57 wrap it currently holds the normalized
// value string, so symbology matching is byte-for-byte unchanged.
struct AttributeValue {
    QString  id;       // e.g. "S57:COLOUR", "S101:colour"
    QVariant value;
};

// How a feature's rings in the GeometryStore should be interpreted.
enum class GeometryKind { Point, Line, Area };

// A slice into ProductFeatureSet::geometry: `ringCount` consecutive rings
// starting at `firstRing`. Keeps coordinates out of the feature record and lets
// products with shared edges converge on one coordinate pool later.
struct GeometryRef {
    GeometryKind kind = GeometryKind::Point;
    quint32 firstRing = 0;
    quint32 ringCount = 0;
};

// Cell-local geometry pool. For the S-57 wrap each feature owns its rings (no
// shared nodes), so this is just their concatenation; S-101 spatial primitives
// and S-57 edge/node topology can converge here without touching features.
struct GeometryStore {
    std::vector<std::vector<Pt>> rings;
};

// A reference to another feature (S-100 information/spatial association). Empty
// for the S-57 wrap today; present so the model can carry S-101 associations.
struct AssociationRef {
    QString  role;       // association role name
    quint64  targetId = 0;
};

// Presentation fields the legacy QPainter/S-52 build step still needs and the
// normalized model does not yet derive. Removed in Stage 4 when portrayal
// computes kind/zorder/fill from FeatureClassId + attributes.
struct LegacyRenderHints {
    FeatureKind kind     = FeatureKind::Point;
    int         zorder   = 0;
    double      depth    = 0.0;
    bool        hasDepth = false;
    std::string name;    // S-57 OBJNAM, label text
};

// One normalized feature.
struct ChartFeature {
    quint64        stableId = 0;     // stable within a cell; for updates/dedup
    FeatureClassId classId;
    GeometryRef    geometry;
    std::vector<AttributeValue> attrs;
    std::vector<AssociationRef> associations;
    int  scaleMin = 0;               // S-57 SCAMIN (0 = always eligible)
    int  displayPriorityHint = 0;
    BBox bbox;
    LegacyRenderHints legacy;        // transitional, see above
};

// Lightweight cell identity for a decoded feature set.
struct ProductCellInfo {
    QString   cellId;                // opaque stable identity (file path for S-57)
    ProductId product;
    BBox      bbox;                  // combined feature extent, projected Mercator
};

// The full normalized decode of one cell: identity + geometry pool + features.
struct ProductFeatureSet {
    ProductCellInfo cell;
    GeometryStore   geometry;
    std::vector<ChartFeature> features;
};
