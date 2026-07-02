#pragma once
// src/gpu_vertex.hpp
//
// One coloured vertex for the retained GPU chart backend, in scene metres
// relative to a scene origin (kept origin-relative so float32 has enough
// precision for Mercator). Split into its own tiny, dependency-free header so
// the batch builder (gpu_batches) and the QRhiWidget (gpu_chart_view) can share
// it without one pulling in the other's heavy includes (QRhiWidget/GuiPrivate).

struct GpuVertex {
    float x, y;
    float r, g, b;
};
