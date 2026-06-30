#pragma once
// src/prepared_render_cache.hpp
//
// Stage 5: on-disk cache of PreparedCellRender (renderer architecture plan,
// Layer 3 "prepared render cache"). This is the third cache level, separate
// from the parsed-cell cache (prepared_chart_cache): it stores the
// portrayal-resolved scene so a cell can be drawn without re-running the S-52
// engine.
//
// It is invalidated independently of the parsed cache. The key adds a
// *portrayal fingerprint* (a digest of the loaded symbols.bin) on top of the
// source-file identity and the decoder/projection/format versions, so changing
// the portrayal package rebuilds only this cache, and changing the chart re-
// derives both.
//
// Threading: load()/store() are pure file IO, designed to run on the build
// worker threads. store() writes atomically (temp file + rename).

#include <QString>
#include "prepared_render.hpp"

namespace prepared_render_cache {

// Load the prepared scene for `sourcePath`. Succeeds only when a cache file
// exists and its key matches the source size/mtime, the code versions, and
// `portrayalFingerprint`. The caller must still verify out.featureCount()
// matches the cell's feature vector before using it (indices are positional).
bool load(const QString& sourcePath, quint64 portrayalFingerprint,
          PreparedCellRender& out);

// Atomically write `prep` as the prepared scene of `sourcePath`. Best effort;
// returns false on IO failure (the caller can proceed without the cache).
bool store(const QString& sourcePath, quint64 portrayalFingerprint,
           const PreparedCellRender& prep);

// Absolute cache directory (for a "clear cache" action).
QString cacheDir();

} // namespace prepared_render_cache
