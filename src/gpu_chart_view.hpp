#pragma once
// src/gpu_chart_view.hpp
//
// Stage 7 (path A): the retained GPU chart canvas.
//
// A QRhiWidget renders through Qt's RHI, targeting Direct3D 11 on Windows. This
// is the rendering core of the future PreparedGpuChartRenderer: it holds retained
// vertex batches (area-fill triangles + line segments) in scene coordinates and
// draws them through a camera uniform, so pan/zoom is a uniform update, not a
// geometry rebuild. Symbols, text, and S-52 niceties come in later increments.
//
// It is intentionally decoupled from how the batches are produced: setScene()
// takes ready-made vertex batches, so it can be exercised by the test harness
// (which builds batches from a cached cell) and later by the app (which builds
// them from a PreparedCellRender). It is not wired into the app yet.

#include <QRhiWidget>
#include <QImage>
#include <QPointF>
#include <cstdint>
#include <memory>
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

    // Install the scene. The draw order matches the painter:
    //   basemap fills -> basemap lines -> raster underlay -> cell fills -> cell lines
    // so the raster (MBTiles) fully covers the basemap but sits beneath the ENC
    // vector charts. Each argument is a vertex list (triangle list for fills, line
    // list of endpoint pairs for lines) in scene metres relative to (originX,
    // originY); the camera is centred on the scene with `ppm` logical px per metre.
    void setScene(std::vector<GpuVertex> baseTris, std::vector<GpuVertex> baseLines,
                  std::vector<GpuVertex> cellTris, std::vector<GpuVertex> cellLines,
                  double centerX, double centerY, double ppm);

    // Move the camera without touching the retained scene — this is the whole
    // point of the retained backend: pan/zoom is a uniform update, not a
    // geometry rebuild. `centerX/Y` are in the same origin-relative frame as the
    // vertices last passed to setScene. Used when the app drives the camera
    // (ChartView owns the touch input); the widget's own pan/zoom handlers stay
    // for the standalone harness.
    void setCamera(double centerX, double centerY, double ppm);

    // Install a raster-chart underlay: one pre-composited image covering the
    // scene rectangle centred on the current scene origin and spanning ±halfW/
    // ±halfH metres. Drawn beneath the vector scene. A null image clears it.
    // (Recomposited by the app when the tile set or view changes, mirroring the
    // painter's static-cache behaviour.)
    void setRasterLayer(const QImage& img, double halfWidthM, double halfHeightM);

    // Cheap one-shot probe: can this machine bring up the RHI backend at all?
    // Creates and destroys a throwaway offscreen RHI (Direct3D 11 on Windows).
    // Used to decide auto-fallback to the painter before any GPU widget is shown,
    // so a broken driver can never blank the chart. Result is cached.
    static bool isAvailable();

protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void render(QRhiCommandBuffer* cb) override;

    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    void releaseResources();

    QRhi* rhi_ = nullptr;
    std::unique_ptr<QRhiBuffer> ubuf_;      // camera uniform (vec4)
    std::unique_ptr<QRhiShaderResourceBindings> srb_;
    std::unique_ptr<QRhiGraphicsPipeline> psTri_;
    std::unique_ptr<QRhiGraphicsPipeline> psLine_;
    std::unique_ptr<QRhiBuffer> vbufBase_;      // basemap fills (below the raster)
    std::unique_ptr<QRhiBuffer> vbufBaseLines_; // basemap outlines (below the raster)
    std::unique_ptr<QRhiBuffer> vbufTris_;      // cell fills (above the raster)
    std::unique_ptr<QRhiBuffer> vbufLines_;     // cell outlines (topmost geometry)

    // Raster underlay (textured quad).
    std::unique_ptr<QRhiGraphicsPipeline> psTex_;
    std::unique_ptr<QRhiShaderResourceBindings> texSrb_;
    std::unique_ptr<QRhiSampler> sampler_;
    std::unique_ptr<QRhiTexture> rasterTex_;
    std::unique_ptr<QRhiBuffer> rasterVbuf_;   // 6 verts (x,y,u,v)
    QImage  rasterImg_;                        // pending upload (null = none)
    double  rasterHalfW_ = 0.0, rasterHalfH_ = 0.0;
    bool    rasterDirty_ = false;              // texture/quad need (re)upload
    bool    hasRaster_ = false;

    std::vector<GpuVertex> baseData_;
    std::vector<GpuVertex> baseLineData_;
    std::vector<GpuVertex> triData_;
    std::vector<GpuVertex> lineData_;
    quint32 baseCount_ = 0;
    quint32 baseLineCount_ = 0;
    quint32 triCount_ = 0;
    quint32 lineCount_ = 0;
    bool sceneDirty_ = false;               // vertex buffers need (re)upload

    // Camera: centre in scene metres (origin-relative) + logical px per metre.
    double camX_ = 0.0, camY_ = 0.0, ppm_ = 1.0;
    QPointF lastDrag_;
    bool dragging_ = false;
};
