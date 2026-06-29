#pragma once
#include <QString>
#include <vector>
#include "chart_loader.hpp"     // Feature, BBox
#include "product_model.hpp"

// ============================================================================
// Stage 3: S-57 legacy <-> normalized model adapter (pure; no GDAL).
// ============================================================================
//
// Bridges the legacy `Feature` vector (what chart::loadCellFeatures emits and
// what the current build/paint path consumes) and the product-neutral
// `ProductFeatureSet`. Kept GDAL-free so it is exercised in tests with synthetic
// features and reused by the S-57 decoder.
//
// The conversion is lossless by construction: every `Feature` field round-trips
// (kind/zorder/depth/name via LegacyRenderHints; objClass/attrs/scaleMin/bbox
// directly; rings moved through the GeometryStore). So a cell decoded to the
// normalized model and adapted back renders byte-for-byte as before.
//
// Note (S-57 wrap limitation, not a model limitation): loadCellFeatures only
// keeps the object-class acronym for symbol-bearing kinds, so FeatureClassId
// ::code is populated only for those; depth/land/coastline features carry an
// empty code. A future decoder that parses S-57 directly can fill it for all.

namespace product_adapter {

// Namespace tag for S-57 feature/attribute identities in the normalized model.
inline constexpr char kS57Ns[] = "S57:";

// Identifies the built-in S-57 ENC product.
ProductId s57ProductId();

// Wrap legacy S-57 features into the normalized model. Geometry is moved into
// the set's GeometryStore (no ring copies); `cellId` is the opaque cell identity
// (the ENC file path). The combined feature extent is computed into
// ProductFeatureSet::cell.bbox.
ProductFeatureSet fromLegacyFeatures(std::vector<Feature> feats,
                                     const QString& cellId);

// Inverse of fromLegacyFeatures: reconstruct the legacy feature vector and set
// `bbox` to the cell extent. Geometry is moved out of `pfs`.
std::vector<Feature> toLegacyFeatures(ProductFeatureSet pfs, BBox& bbox);

} // namespace product_adapter
