#pragma once
#include <QString>
#include "product_model.hpp"

// ============================================================================
// Stage 3: chart-product decoders (renderer architecture plan, Layer 1).
// ============================================================================
//
// A product decoder reads one chart product and emits the normalized
// ProductFeatureSet. It does not resolve symbols and does not create GPU
// objects. S-57 is the first decoder; S-101/CM93/raster decoders implement the
// same interface later, so the rest of the pipeline never learns how a cell was
// encoded.
//
// Catalog/coverage scanning is out of scope here — that path (ChartCatalog,
// chart::computeCellCoverage) is unchanged for Stage 3. This interface covers
// only per-cell decoding.

class IChartProductDecoder {
public:
    virtual ~IChartProductDecoder() = default;

    // Which product this decoder handles.
    virtual ProductId productId() const = 0;

    // Decode one cell (identified by `cellId`) into the normalized model. Runs
    // on a worker thread and must be thread-safe across concurrent calls for
    // different cells. Returns false and sets `err` on failure.
    virtual bool loadCell(const QString& cellId, ProductFeatureSet& out,
                          QString& err) = 0;
};

// Built-in S-57 ENC decoder. Wraps the existing GDAL reader
// (chart::loadCellFeatures) and converts its output into the normalized model
// via product_adapter. `cellId` is the ENC cell file path.
class S57ProductDecoder : public IChartProductDecoder {
public:
    ProductId productId() const override;
    bool loadCell(const QString& cellId, ProductFeatureSet& out,
                  QString& err) override;
};
