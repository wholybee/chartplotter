#include "product_decoder.hpp"
#include "product_adapter.hpp"
#include "chart_loader.hpp"

#include <string>
#include <vector>

ProductId S57ProductDecoder::productId() const {
    return product_adapter::s57ProductId();
}

bool S57ProductDecoder::loadCell(const QString& cellId, ProductFeatureSet& out,
                                 QString& err) {
    std::vector<Feature> feats;
    BBox bbox;
    std::string e;
    if (!chart::loadCellFeatures(cellId.toStdString(), feats, bbox, e)) {
        err = QString::fromStdString(e);
        return false;
    }
    out = product_adapter::fromLegacyFeatures(std::move(feats), cellId);
    return true;
}
