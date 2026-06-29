#pragma once
#include <QString>
#include <vector>
#include "chart_loader.hpp"   // Feature, BBox, Pt

// ============================================================================
// Stage 1: binary parsed-cell cache.
// ============================================================================
//
// Parsing an ENC cell is the dominant cold-load cost: a worker opens the cell
// with GDAL, walks every S-57 layer, and projects all geometry into Mercator
// metres (chart::loadCellFeatures). The result is plain data — a
// std::vector<Feature> plus the cell bbox — so once parsed it can be written to
// disk and reloaded directly, skipping GDAL entirely on the next run.
//
// This is the parsed-product cache level of the renderer architecture plan
// (docs/renderer_architecture_plan.md). It benefits the current renderer today
// and is reused by the future retained renderer.
//
// Identity / invalidation. A cached artifact is keyed by the source file's
// absolute path plus its size and modification time, and by three code
// versions: the decoder, the projection, and the on-disk format. If the source
// file changes, or any of those versions is bumped, load() reports a miss and
// the caller re-parses (then re-stores). Deleting the cache directory is always
// safe.
//
// Threading. load()/store() are pure file IO and are designed to run on the
// load worker threads (off the UI thread). Concurrent calls for *different*
// cells are safe. store() writes atomically (temp file + rename) so a reader
// never observes a torn file, and two workers racing on the same cell at worst
// rewrite the same bytes.
//
// Scope. Only the built-in GDAL path is cached for now: its cell id is a real
// file path whose size/mtime give a cheap, reliable validity check. Plugin
// IChartSource backends (whose cell ids are opaque) can opt in later.

namespace prepared_cache {

// Try to load a previously cached parse of `sourcePath`. Returns true and fills
// `out`/`bbox` only when a cache file exists and its key matches the source
// file's current size/mtime and the current code versions. Any mismatch,
// missing file, or read error returns false (a miss) and leaves the outputs
// untouched.
bool load(const QString& sourcePath, std::vector<Feature>& out, BBox& bbox);

// Cheap freshness probe: true when a cache file for `sourcePath` exists and its
// key (versions + source size/mtime) is currently valid, without deserializing
// the feature payload. Used by the "Prepare chart cache" pass to skip cells
// already prepared. A false result means load() would miss.
bool isFresh(const QString& sourcePath);

// Atomically write `feats`/`bbox` as the cached parse of `sourcePath`. Best
// effort: returns false on IO failure but the caller can ignore it (the parse
// already succeeded; the cache is an optimization).
bool store(const QString& sourcePath, const std::vector<Feature>& feats,
           const BBox& bbox);

// Absolute path of the cache directory (created lazily by store()). Exposed so
// a "clear cache" UI action can remove it.
QString cacheDir();

} // namespace prepared_cache
