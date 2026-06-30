#pragma once
// src/sym_atlas.hpp
//
// Facade over the split symbology subsystem (Stage 4). The portrayal/render
// responsibilities that used to live in this one class are now three
// collaborators:
//
//   PortrayalPackage     S-52 lookup tables, conditions, colours, instruction
//                        blob — the swappable rule/style data.
//   PortrayalEngine      evaluates a feature against the package and emits a
//                        renderer-neutral SymHit (portrayal_ir.hpp).
//   RenderResourceAtlas  the atlas pixmap, LC/AP resources, and the QPainter
//                        draw helpers.
//
// SymAtlas keeps the original public API so ChartView and the build/paint path
// are unchanged: it loads symbols.bin, hands each section to the package /
// resource atlas, and forwards queries and drawing to the right collaborator.
// Swapping the portrayal package or the resource atlas (e.g. for S-101) now
// touches neither the chart decoders nor the renderer batches.
//
// Thread safety: load() runs once on the GUI thread before any worker calls
// symbolForFeature(); afterwards the data is immutable and symbolForFeature()
// is safe to call concurrently. The draw helpers run on the GUI thread.

#include <QByteArray>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "portrayal_ir.hpp"
#include "portrayal_engine.hpp"
#include "render_resource_atlas.hpp"

class SymAtlas {
public:
    static constexpr uint16_t kNoSymbol = ::kNoSymbol;

    // A feature's symbology-relevant attributes: (6-char acronym, value string).
    using AttrList = PortrayalAttrs;

    bool load(const QString& binPath, const QString& pngPath);
    bool isLoaded() const { return resources_.isLoaded(); }

    // Identity digest of the loaded portrayal package (derived from symbols.bin's
    // size + mtime). Used to key the prepared-render cache so swapping the
    // portrayal package invalidates it. 0 until a successful load().
    quint64 fingerprint() const { return fingerprint_; }

    // The S-57 attribute acronyms that any lookup condition, text command, or CS
    // procedure references. The chart loader reads exactly these per feature.
    const std::vector<std::string>& relevantAttrs() const {
        return package_.relevantAttrs();
    }

    // Resolve a feature (object class + geometry + attributes) to a SymHit via
    // S-52 best-match selection plus instruction execution (including CS).
    SymHit symbolForFeature(const QByteArray& objClass, SymGeom geom,
                            const AttrList& attrs) const {
        return PortrayalEngine(package_, resources_).evaluate(objClass, geom, attrs);
    }

    // Resolve a symbol name (e.g. "BOYPIL61") to its atlas index.
    uint16_t findSymbol(const QByteArray& name) const {
        return resources_.findSymbol(name);
    }

    // Draw symbol symIdx at screen point d, honouring the pivot offset.
    void draw(QPainter& p, uint16_t symIdx, QPointF d,
              float rotationDeg = 0.0f, float scale = 1.0f) const {
        resources_.draw(p, symIdx, d, rotationDeg, scale);
    }

    // Stamp LC line-complex `lcIndex` repeatedly along a device-space polyline.
    void drawLineComplex(QPainter& p, int lcIndex,
                         const QPolygonF& devicePts, float scale) const {
        resources_.drawLineComplex(p, lcIndex, devicePts, scale);
    }

    // Fill `deviceClipPath` (device coords) with AP pattern `apIndex`.
    void fillAreaPattern(QPainter& p, int apIndex,
                         const QPainterPath& deviceClipPath,
                         QPointF anchor, float scale) const {
        resources_.fillAreaPattern(p, apIndex, deviceClipPath, anchor, scale);
    }

    bool hasLineComplex(int i) const { return resources_.hasLineComplex(i); }
    bool hasAreaPattern(int i) const { return resources_.hasAreaPattern(i); }

private:
    PortrayalPackage    package_;
    RenderResourceAtlas resources_;
    quint64             fingerprint_ = 0;
};
