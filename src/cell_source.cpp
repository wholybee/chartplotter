// src/cell_source.cpp
#include "cell_source.hpp"

#include <QElapsedTimer>
#include <QLoggingCategory>

#include "chart_source.hpp"          // IChartSource
#include "prepared_chart_cache.hpp"  // prepared_cache::load / store
#include "product_decoder.hpp"       // S57ProductDecoder
#include "product_adapter.hpp"       // product_adapter::toLegacyFeatures

namespace {
// Parsed-cell cache hit/miss + parse timing. Quiet by default; enable with
// QT_LOGGING_RULES="chart.cache.debug=true".
Q_LOGGING_CATEGORY(lcCache, "chart.cache")
} // namespace

namespace cellsource {

CellLoadResult parseCell(const QString& path, IChartSource* src) {
    CellLoadResult r;
    r.path = path;
    if (src) {
        // Plugin backend: the cell id is the QString path; geometry comes back
        // already projected, with S-57 object classes/attrs for symbology.
        // (Opaque cell ids aren't cached yet — see prepared_chart_cache.hpp.)
        QString err;
        r.ok = src->loadCell(path, r.features, r.bbox, err);
        r.error = err;
    } else {
        // Built-in GDAL path. Try the on-disk parsed-cell cache first; on a
        // miss, parse with GDAL and write the cache for next time.
        if (prepared_cache::load(path, r.features, r.bbox)) {
            r.ok = true;
            qCDebug(lcCache) << "hit" << path << r.features.size() << "features";
        } else {
            // Decode through the normalized product model (Stage 3): the
            // S-57 decoder emits a ProductFeatureSet, which the compatibility
            // adapter converts back to the legacy Feature vector the build/
            // paint path and the parsed-cell cache still consume.
            QElapsedTimer t;
            t.start();
            S57ProductDecoder decoder;
            ProductFeatureSet pfs;
            QString err;
            r.ok = decoder.loadCell(path, pfs, err);
            if (r.ok) {
                r.features = product_adapter::toLegacyFeatures(std::move(pfs), r.bbox);
                qCDebug(lcCache) << "miss" << path << "parsed in" << t.elapsed()
                                 << "ms," << r.features.size() << "features";
                prepared_cache::store(path, r.features, r.bbox);
            } else {
                r.error = err;
            }
        }
    }
    return r;
}

} // namespace cellsource
