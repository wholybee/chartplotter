#pragma once
// src/gpu_chart_view.hpp
//
// Stage 7: the retained GPU chart canvas.
//
// A QRhiWidget renders through Qt's RHI, targeting Direct3D 11 on Windows. It
// holds one retained vertex-buffer set per chart cell (fill triangles + line
// segments + depth-contour segments), each uploaded once when the cell is
// built and freed from CPU memory immediately after. Every draw applies the
// cell's own origin through a per-draw camera uniform (dynamic uniform-buffer
// offsets), so:
//   - pan/zoom is a uniform update — no vertex copying, no re-upload;
//   - float32 precision never needs a scene re-base (vertices stay relative to
//     their cell-local origin; the origin-minus-camera offset is computed in
//     double per frame);
//   - scene changes (quilt updates, toggles) are draw-list edits;
//   - cells are culled individually against the viewport.
//
// It is intentionally decoupled from how the batches are produced: setCell()
// takes ready-made vertex batches, so it can be exercised by the test harness
// (which builds batches from a cached cell) and by the app (which builds them
// from a PreparedCellRender + BuiltCell via gpubatches).

#include <QRhiWidget>
#include <QImage>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QHashFunctions>   // std::hash<QString>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include "gpu_vertex.hpp"   // GpuVertex

QT_BEGIN_NAMESPACE
class QRhi;
class QRhiBuffer;
class QRhiTexture;
class QRhiSampler;
class QRhiGraphicsPipeline;
class QRhiShaderResourceBindings;
QT_END_NAMESPACE

class GpuChartView : public QRhiWidget {
    Q_OBJECT
public:
    explicit GpuChartView(QWidget* parent = nullptr);
    ~GpuChartView() override;

    // Install or replace one retained cell. Vertices are projected metres
    // relative to (originX, originY), which is absolute (wrap offset folded
    // in); min/max bound the cell's geometry in absolute projected metres for
    // per-cell viewport culling. Buffers are (re)uploaded on the next frame and
    // the passed vectors are freed right after — the GPU copy is then the only
    // copy. A cell not in the draw list is retained but not drawn.
    void setCell(const QString& key,
                 std::vector<GpuVertex> tris,
                 std::vector<GpuVertex> lines,
                 std::vector<GpuVertex> contours,
                 double originX, double originY,
                 double minX, double minY, double maxX, double maxY);
    void removeCell(const QString& key);
    void clearCells();

    // Draw order, coarse-band-first within each list. `baseKeys` (the basemap)
    // draw beneath the raster underlay; `cellKeys` (chart cells) above it.
    // drawFills=false suppresses fill triangles (vector-overlay mode);
    // drawContours=false suppresses the depth-contour buckets.
    void setDrawList(const QStringList& baseKeys, const QStringList& cellKeys,
                     bool drawFills, bool drawContours);

    // Move the camera without touching the retained cells — pan/zoom is a
    // uniform update, not a geometry rebuild. `centerX/centerY` are absolute
    // projected metres (Y north-up); per-cell origins are subtracted per draw.
    // `upBearingDeg` rotates the scene so that compass bearing points to the top
    // of the view (0 = north-up); it applies in the vertex shader, so it too is a
    // uniform-only update.
    void setCamera(double centerX, double centerY, double ppm,
                   double upBearingDeg = 0.0);

    // --- Raster (MBTiles) tile layer ---------------------------------------
    // One retained texture per tile, drawn as textured quads above the basemap
    // and beneath the chart cells. Quad extents are absolute projected metres
    // (Y north-up, x0<x1, y0<y1); u/v select the source sub-rect so a coarser
    // ancestor tile can stand in for a missing one.
    struct TileQuad {
        quint64 texId = 0;
        double  x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
        float   u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
        bool operator==(const TileQuad& o) const {
            return texId == o.texId && x0 == o.x0 && y0 == o.y0 && x1 == o.x1 &&
                   y1 == o.y1 && u0 == o.u0 && v0 == o.v0 && u1 == o.u1 && v1 == o.v1;
        }
    };
    // Replace the drawn tile set (in draw order; an empty vector clears the
    // layer). Unchanged sets are a no-op. Retained textures beyond the budget
    // are shed once they leave the drawn set.
    void setRasterTiles(std::vector<TileQuad> quads);
    // Texture cache: the owner uploads a tile's image once (setTileTexture)
    // and reuses it across selections while hasTileTexture stays true.
    bool hasTileTexture(quint64 texId) const;
    void setTileTexture(quint64 texId, const QImage& img);

    // Cheap one-shot probe: can this machine bring up the RHI backend the widget
    // will actually use (Direct3D 11 on Windows, Metal on macOS, OpenGL elsewhere)?
    // Creates and destroys a throwaway offscreen device. Used to decide auto-
    // fallback to the painter before any GPU widget is shown, so a broken driver
    // can never blank the chart. Result is cached.
    static bool isAvailable();

    // Frame telemetry: RHI frames rendered, cell-buffer uploads, raster texture
    // uploads, and vertices drawn last frame (after culling). Read-and-reset
    // once per second by ChartView's telemetry dump.
    void takeTelemetry(int& frames, int& sceneUploads, int& textureUploads,
                       quint64& vertsDrawn);

    // Monotonic count of frames the RHI has actually rendered since construction.
    // Never reset (unlike the telemetry counter), so ChartView's fallback watchdog
    // can tell whether the device produced any output after the widget was shown:
    // a value that never advances past the moment of show means the GPU device
    // came up dead and the view must fall back to the CPU painter.
    quint64 renderedFrames() const { return renderedFrames_; }

signals:
    // The QRhi was torn down and recreated (window/screen change, device loss).
    // Every retained buffer died with it and the CPU copies were freed at
    // upload, so the owner must re-push all cells (rebuild from cached
    // features). Emitted queued, never during a render callback's caller.
    void deviceLost();

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;

    // Force a render (and therefore a backing-store recomposite) every time the
    // widget becomes visible. Without this the very first RHI frame relies on
    // Qt's implicit expose paint, which can race the parent's first composite of
    // the translucent overlay stacked on top — leaving the chart black until some
    // later event happens to schedule a repaint. See the .cpp for the full story.
    void showEvent(QShowEvent* e) override;

    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    struct CellEntry {
        std::unique_ptr<QRhiBuffer> vbufTris, vbufLines, vbufContours;
        quint32 triCount = 0, lineCount = 0, contourCount = 0;
        // Batches waiting for upload on the next frame; freed once uploaded.
        std::vector<GpuVertex> pendTris, pendLines, pendContours;
        bool pending = false;
        double originX = 0.0, originY = 0.0;    // absolute projected metres
        double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;   // culling bounds
    };

    void releaseResources();
    // Grow the per-draw uniform buffer (and rebind the SRBs) to hold at least
    // `slotCount` slices. Cheap no-op when capacity suffices. (NB: not named
    // "slots" — that's a Qt keyword macro.)
    void ensureUniformCapacity(int slotCount);
    // A shader-resource-binding set for the textured pipeline: the shared
    // per-draw camera uniform plus one sampled texture.
    std::unique_ptr<QRhiShaderResourceBindings> makeTexSrb(QRhiTexture* t);

    QRhi* rhi_ = nullptr;
    // Per-draw camera uniform: one aligned vec4 slice per draw, addressed with
    // dynamic offsets. Slice = { camX-originX, camY-originY, ndcPerMetreX/Y }.
    std::unique_ptr<QRhiBuffer> ubuf_;
    int     ubufSlots_ = 0;
    quint32 slotStride_ = 0;
    std::unique_ptr<QRhiShaderResourceBindings> srb_;
    std::unique_ptr<QRhiGraphicsPipeline> psTri_;
    std::unique_ptr<QRhiGraphicsPipeline> psLine_;

    // Raster tile layer: textured pipeline (psTex_ + a 1x1 placeholder texture
    // whose SRB donates the pipeline layout), one retained texture + SRB per
    // tile, and one shared quad vertex buffer rebuilt when the drawn set
    // changes. Quad verts are relative to tileOrigin* (the camera at rebuild
    // time — refreshed by every settle reselection, so float32 holds).
    std::unique_ptr<QRhiGraphicsPipeline> psTex_;
    std::unique_ptr<QRhiShaderResourceBindings> texSrb_;
    std::unique_ptr<QRhiSampler> sampler_;
    std::unique_ptr<QRhiTexture> rasterTex_;   // 1x1 layout placeholder
    struct TileTexEntry {
        std::unique_ptr<QRhiTexture> tex;
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        QImage pending;   // waiting for upload (null once uploaded)
    };
    std::unordered_map<quint64, TileTexEntry> tileTex_;
    std::vector<TileQuad> tileQuads_;
    std::unique_ptr<QRhiBuffer> tileVbuf_;
    quint32 tileVbufCap_ = 0;        // capacity, in quads
    bool    tileQuadsDirty_ = false;
    double  tileOriginX_ = 0.0, tileOriginY_ = 0.0;

    // Retained cells + the current draw order/options. (std::unordered_map, not
    // QHash: CellEntry holds unique_ptrs and QHash needs copyable values.)
    std::unordered_map<QString, CellEntry> cells_;
    QStringList baseList_, cellList_;
    bool drawFills_ = true;
    bool drawContours_ = true;
    bool firstInit_ = true;

    // Monotonic frame count for the fallback watchdog (see renderedFrames()).
    quint64 renderedFrames_ = 0;

    // Telemetry counters (see takeTelemetry).
    int telemFrames_ = 0;
    int telemSceneUploads_ = 0;
    int telemTexUploads_ = 0;
    quint64 telemVertsDrawn_ = 0;

    // Camera: centre in absolute projected metres + logical px per metre.
    double camX_ = 0.0, camY_ = 0.0, ppm_ = 1.0;
    double camUpDeg_ = 0.0;              // scene rotation: bearing pointing up
    double camCos_ = 1.0, camSin_ = 0.0; // cached cos/sin of camUpDeg_
    QPointF lastDrag_;
    bool dragging_ = false;
};
