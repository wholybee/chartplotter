#pragma once
// src/cell_source.hpp
//
// Backend-neutral cell parsing: turn a cell path (built-in ENC via GDAL, or a
// plugin IChartSource) into the parsed Feature vector + bbox. Extracted from
// ChartView's load worker (Stage 7 A3) so the painter and the retained GPU
// backend share one loader instead of each duplicating the cache/decoder dance.
//
// Pure and Qt-value-only: no widget or camera state, safe to run on a worker
// thread. The async orchestration (thread pool, generation guard, in-flight
// bookkeeping) stays on each backend's shell; this is just the work a worker
// performs.

#include <QString>
#include <vector>

#include "chart_loader.hpp"   // Feature, BBox

class IChartSource;

// Result of parsing one cell on a worker thread (also the QFuture result type
// the load watchers carry).
struct CellLoadResult {
    QString path;
    std::vector<Feature> features;
    BBox bbox;
    bool ok = false;
    QString error;
};

namespace cellsource {

// Parse one cell. With a non-null `src` (e.g. a CM93 plugin) the cell id is the
// QString path and geometry comes back already projected. Otherwise the built-in
// path is used: the on-disk parsed-cell cache first, else a GDAL/S-57 decode
// (through the normalized product model) whose result is written back to the
// cache. `path` is echoed into the result so a caller can key the reply.
CellLoadResult parseCell(const QString& path, IChartSource* src);

} // namespace cellsource
