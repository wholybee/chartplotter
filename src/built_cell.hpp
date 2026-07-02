#pragma once
// src/built_cell.hpp
//
// Ready-to-draw, view-instantiated chart-cell primitives. Extracted from
// chart_view.hpp (Stage 7 A3) so the shared cell builder (cell_builder) and a
// future ChartEngine can produce and hold them without depending on the
// ChartView widget. Coordinates are scene metres: projected Mercator with Y
// negated so north is up. All types are Qt value objects, safe to build on a
// worker thread and draw on the GUI thread.

#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <cstdint>
#include <vector>

#include "chart_loader.hpp"   // BBox
#include "portrayal_ir.hpp"   // kNoSymbol
#include "gpu_vertex.hpp"     // GpuVertex (retained GPU batches)

// One ready-to-draw vector primitive: a clipped, simplified path plus its style.
struct BuiltPath {
    QPainterPath path;
    QRectF bounds;             // path.boundingRect(), for view culling
    double z = 0.0;
    bool   filled = false;     // true: area (use brush); false: line (pen only)
    QColor brush;
    bool   hasPen = false;
    QColor penColor;
    qreal  penWidth = 1.0;
    Qt::PenStyle penStyle = Qt::SolidLine;   // SolidLine / DashLine / DotLine
    bool   isDepthContour = false;
    // S-52 complex symbology resolved at build time, rendered at constant on-
    // screen size in device space (see paintEvent). -1 when absent.
    int    apIndex = -1;       // AP() area-pattern fill (tiled motif)
    int    lcIndex = -1;       // LC() complex line (motif stamped along the path)
    int    scaleMin = 0;       // S-57 SCAMIN (0 = none); paint-time declutter floor
};

// A single sounding: scene position plus the raw depth in metres (S-57's native
// unit). The label is formatted at paint time from the current depth unit, so
// switching feet/metres is a repaint — no rebuild of the cell geometry.
struct Sounding {
    QPointF pos;            // scene position (Y already flipped north-up)
    double  depthM = 0.0;   // raw depth, metres
    bool    hasDepth = false;
    int     scaleMin = 0;   // S-57 SCAMIN (0 = none); paint-time declutter floor
};

// A resolved symbol: scene position + atlas index + optional rotation.
// symIdx == kNoSymbol means no atlas entry was found; the renderer falls back to
// the magenta dot used before symbol support was added. rotationDeg is the S-57
// ORIENT angle (degrees CW from true north); zero for upright symbols.
struct BuiltSymbol {
    QPointF  pos;
    uint16_t symIdx = kNoSymbol;
    float    rotationDeg = 0.0f;
    int      scaleMin = 0;   // S-57 SCAMIN (0 = none); paint-time declutter floor
};

// A text label drawn at constant on-screen size next to its object.
//   hjust 1=centre 2=right 3=left   vjust 1=bottom 2=centre 3=top
// xoffs/yoffs are in nominal character-box units (≈ font width/height).
struct BuiltText {
    QPointF pos;             // scene position of the object the label annotates
    QString text;
    int     scaleMin = 0;    // S-57 SCAMIN (0 = none)
    quint8  hjust = 3, vjust = 2;
    int     xoffs = 0, yoffs = 0;
    QColor  color = QColor(40, 40, 40);
    quint8  pointSize = 8;
};

// A light sector: a coloured arc plus its two dashed limit legs, drawn at
// constant on-screen size around a sectored light. Bearings are directions from
// the light (degrees CW from true north); the lit arc sweeps clockwise
// startDeg→endDeg.
struct BuiltLightSector {
    QPointF pos;             // scene position of the light (Y flipped north-up)
    float   startDeg = 0.0f;
    float   endDeg   = 0.0f;
    float   rangeNm  = 0.0f;
    QColor  color;
    int     scaleMin = 0;    // S-57 SCAMIN (0 = none)
};

// A whole cell, clipped to a region and ready to draw. drawOffsetX shifts it by a
// whole-world width so cells near the date line can be drawn on the far side of
// the 180° seam (longitude wrap-around).
struct BuiltCell {
    QString path;
    int  band = 0;
    BBox clipBox;                                          // region (real frame)
    double drawOffsetX = 0.0;                              // scene-X wrap offset
    std::vector<BuiltPath>   paths;                        // sorted by z
    std::vector<Sounding>    soundings;                    // scene pos + depth
    std::vector<BuiltSymbol> symbols;                      // scene pos + sym idx
    std::vector<BuiltText>   texts;                        // scene pos + label
    std::vector<BuiltLightSector> sectors;                 // light sector arcs

    // Retained GPU batches (Stage 7 A4), populated by the build worker only when
    // the GPU backend is active. Vertices are in the projected frame (Y
    // north-up-positive, unlike the QPainter primitives above) relative to the
    // cell-local origin (gpuOriginX/Y), so float32 keeps precision; the view
    // re-bases them to a common scene origin when it assembles the frame.
    std::vector<GpuVertex> gpuTris;
    std::vector<GpuVertex> gpuLines;
    double gpuOriginX = 0.0;
    double gpuOriginY = 0.0;
};
