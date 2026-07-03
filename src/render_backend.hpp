#pragma once
#include <QString>

// Stage 7: which chart-rendering backend to use.
//
//   Auto - use the GPU (retained, QRhiWidget/Direct3D) backend when it is
//          available and initializes successfully; otherwise fall back to the
//          CPU painter backend automatically.
//   Gpu  - force the GPU backend (debug/diagnostic). Still falls back to CPU if
//          the device cannot be created, so a broken driver can never blank the
//          chart.
//   Cpu  - always use the QPainter backend (the safe choice if the GPU path
//          misbehaves on a particular machine's drivers).
//
// The user-facing control is a simple "Use GPU acceleration" toggle mapping to
// Gpu (on) / Cpu (off); the three-way enum is kept for diagnostics and future
// UI. The retained GPU backend has not yet passed its performance/parity gates
// (docs/performance_fix_plan.md, Step 4.5), so Auto currently resolves to CPU
// and only an explicit Gpu preference opts in for testing. Flip Auto back to
// GPU-when-available once the gates pass.
enum class RenderBackend { Auto, Gpu, Cpu };

namespace chartrender {

inline QString key(RenderBackend b) {
    switch (b) {
        case RenderBackend::Gpu: return QStringLiteral("gpu");
        case RenderBackend::Cpu: return QStringLiteral("cpu");
        case RenderBackend::Auto: break;
    }
    return QStringLiteral("auto");
}

inline RenderBackend fromKey(const QString& k, RenderBackend fallback) {
    if (k == QLatin1String("gpu")) return RenderBackend::Gpu;
    if (k == QLatin1String("cpu")) return RenderBackend::Cpu;
    if (k == QLatin1String("auto")) return RenderBackend::Auto;
    return fallback;
}

// Decide whether to actually use the GPU backend, given the user's preference
// and whether a GPU backend is built and reported its device as usable. This is
// the single auto-fallback decision point: a Cpu preference never uses the GPU,
// Auto resolves to the CPU painter until the GPU backend passes its gates
// (docs/performance_fix_plan.md, Step 4.5), and an explicit Gpu preference uses
// the GPU only when `gpuAvailable` is true — so an unavailable or failed GPU
// device always lands on the CPU painter backend.
inline bool resolveUseGpu(RenderBackend pref, bool gpuAvailable) {
    if (pref != RenderBackend::Gpu) return false;
    return gpuAvailable;   // explicit opt-in still requires a usable device
}

} // namespace chartrender
