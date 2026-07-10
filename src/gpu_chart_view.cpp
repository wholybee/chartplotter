// src/gpu_chart_view.cpp
#include "gpu_chart_view.hpp"

#include <rhi/qrhi.h>
#include <QColor>
#include <QFile>
#include <QMouseEvent>
#include <QShowEvent>
#include <QWheelEvent>
#include <QSize>
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace {

QShader loadShader(const QString& name) {
    QFile f(name);
    if (f.open(QIODevice::ReadOnly))
        return QShader::fromSerialized(f.readAll());
    return QShader();
}

constexpr int kStride = 5 * sizeof(float);       // x,y,r,g,b
constexpr int kCamSlice = 8 * sizeof(float);     // vec4 cam + vec4 rot
constexpr int kTileVertFloats = 6 * 4;           // one quad: 6 verts × (x,y,u,v)
constexpr std::size_t kMaxTileTextures = 384;    // matches the pixmap cache budget

} // namespace

// Info-level so it is enabled by default (never filtered) and always reaches the
// app_log message handler; see gpu_chart_view.hpp.
Q_LOGGING_CATEGORY(lcGpu, "hmv.gpu", QtInfoMsg)

GpuChartView::GpuChartView(QWidget* parent) : QRhiWidget(parent) {
#if defined(Q_OS_WIN)
    setApi(QRhiWidget::Api::Direct3D11);
    qCInfo(lcGpu).noquote() << "GpuChartView constructed (api=Direct3D11)";
#elif defined(Q_OS_MACOS)
    // Leave the platform default (Metal).
    qCInfo(lcGpu).noquote() << "GpuChartView constructed (api=default/Metal)";
#else
    // Linux/Raspberry Pi: pin OpenGL so the widget requests the same backend the
    // window's RHI backing store uses, ruling out an API mismatch as a cause of
    // "QRhiWidget: No QRhi".
    setApi(QRhiWidget::Api::OpenGL);
    qCInfo(lcGpu).noquote() << "GpuChartView constructed (api=OpenGL)";
#endif
}

GpuChartView::~GpuChartView() = default;

void GpuChartView::setCell(const QString& key,
                           std::vector<GpuVertex> tris,
                           std::vector<GpuVertex> lines,
                           std::vector<GpuVertex> contours,
                           double originX, double originY,
                           double minX, double minY, double maxX, double maxY) {
    CellEntry& c = cells_[key];
    c.pendTris     = std::move(tris);
    c.pendLines    = std::move(lines);
    c.pendContours = std::move(contours);
    c.pending      = true;
    c.originX = originX;
    c.originY = originY;
    c.minX = minX; c.minY = minY; c.maxX = maxX; c.maxY = maxY;
    update();
}

void GpuChartView::removeCell(const QString& key) {
    if (cells_.erase(key) > 0) update();
}

void GpuChartView::clearCells() {
    if (cells_.empty()) return;
    cells_.clear();
    update();
}

void GpuChartView::setDrawList(const QStringList& baseKeys, const QStringList& cellKeys,
                               bool drawFills, bool drawContours) {
    if (baseList_ == baseKeys && cellList_ == cellKeys &&
        drawFills_ == drawFills && drawContours_ == drawContours)
        return;   // no-op guard: unchanged draw list must not schedule a frame
    baseList_ = baseKeys;
    cellList_ = cellKeys;
    drawFills_ = drawFills;
    drawContours_ = drawContours;
    update();
}

void GpuChartView::setCamera(double centerX, double centerY, double ppm,
                             double upBearingDeg) {
    const double p = (ppm > 0.0) ? ppm : 1.0;
    // No-op guard: an unchanged camera must not schedule an RHI frame. Data-driven
    // repaints (AIS/ownship) re-push the same camera every governor tick; without
    // this the retained scene is re-rendered for frames where nothing moved, and
    // any accidental repaint loop feeds itself instead of dying out.
    if (centerX == camX_ && centerY == camY_ && p == ppm_ && upBearingDeg == camUpDeg_)
        return;
    camX_ = centerX;
    camY_ = centerY;
    ppm_  = p;
    camUpDeg_ = upBearingDeg;
    const double rad = upBearingDeg * 0.017453292519943295;   // deg -> rad
    camCos_ = std::cos(rad);
    camSin_ = std::sin(rad);
    update();   // uniform-only refresh; no geometry rebuild
}

void GpuChartView::setRasterTiles(std::vector<TileQuad> quads) {
    if (quads == tileQuads_) return;   // no-op guard: unchanged set, no frame
    tileQuads_ = std::move(quads);
    tileQuadsDirty_ = true;
    // Texture budget: once over it, shed retained textures the drawn set no
    // longer references (the owner re-uploads from its pixmap cache if one is
    // ever needed again).
    if (tileTex_.size() > kMaxTileTextures) {
        std::unordered_set<quint64> used;
        used.reserve(tileQuads_.size());
        for (const TileQuad& q : tileQuads_) used.insert(q.texId);
        for (auto it = tileTex_.begin();
             it != tileTex_.end() && tileTex_.size() > kMaxTileTextures; ) {
            if (used.count(it->first)) ++it;
            else                       it = tileTex_.erase(it);
        }
    }
    update();
}

bool GpuChartView::hasTileTexture(quint64 texId) const {
    return tileTex_.count(texId) > 0;
}

void GpuChartView::setTileTexture(quint64 texId, const QImage& img) {
    if (img.isNull()) return;
    tileTex_[texId].pending =
        img.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    update();   // texture is created + uploaded on the next frame
}

bool GpuChartView::isAvailable() {
    // This used to pre-flight the GPU by bringing up and tearing down a throwaway
    // RHI device before the widget was ever shown, so a broken driver could be
    // caught early. That backfired: it doubled device create/destroy churn at
    // startup (the probe device, then the widget's own real device moments
    // later), and on some drivers (Raspberry Pi V3D/Mesa; certain Windows
    // drivers) the two racing inits could bring the *real* device up dead — a
    // random black chart that no in-process action could recover, because the
    // widget's device is created once and reused for the process lifetime.
    //
    // Device validation now happens at runtime instead: ChartView arms a watchdog
    // when it shows the GPU layer and falls back to the CPU painter if the RHI
    // produces no frame (see ChartView::checkGpuWatchdog). That is strictly more
    // accurate (it tests the device the widget actually uses, in situ) and adds
    // no extra device creation. So this now only reports that the GPU backend is
    // compiled in; the runtime watchdog is the real gate.
    return true;
}

void GpuChartView::takeTelemetry(int& frames, int& sceneUploads, int& textureUploads,
                                 quint64& vertsDrawn) {
    frames         = telemFrames_;       telemFrames_       = 0;
    sceneUploads   = telemSceneUploads_; telemSceneUploads_ = 0;
    textureUploads = telemTexUploads_;   telemTexUploads_   = 0;
    vertsDrawn     = telemVertsDrawn_;   // last frame's count, not accumulated
}

void GpuChartView::releaseResources() {
    if (rhi_) qCInfo(lcGpu).noquote()
                  << "releaseResources: RHI torn down (device loss or shutdown)";
    psTri_.reset();
    psLine_.reset();
    srb_.reset();
    ubuf_.reset();
    ubufSlots_ = 0;
    psTex_.reset();
    texSrb_.reset();
    sampler_.reset();
    rasterTex_.reset();
    tileTex_.clear();
    tileVbuf_.reset();
    tileVbufCap_ = 0;
    tileQuadsDirty_ = true;
    // Per-cell GPU buffers died with the RHI. Entries still holding pending
    // CPU batches survive (they upload on the next frame — including cells
    // pushed before the very first frame); entries whose CPU copy was already
    // freed at upload are unrecoverable and are dropped — the owner re-pushes
    // them on deviceLost().
    for (auto it = cells_.begin(); it != cells_.end(); ) {
        CellEntry& c = it->second;
        c.vbufTris.reset();
        c.vbufLines.reset();
        c.vbufContours.reset();
        c.triCount = c.lineCount = c.contourCount = 0;
        if (c.pending) ++it;
        else           it = cells_.erase(it);
    }
}

void GpuChartView::ensureUniformCapacity(int slotCount) {
    if (slotCount <= ubufSlots_ && ubuf_) return;
    int want = std::max(64, ubufSlots_);
    while (want < slotCount) want *= 2;

    ubuf_.reset(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                want * slotStride_));
    ubuf_->create();
    ubufSlots_ = want;

    // Rebind the SRBs to the new buffer object. The layout is unchanged, so the
    // existing pipelines (which only captured the layout) stay valid.
    srb_.reset(rhi_->newShaderResourceBindings());
    srb_->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
            0, QRhiShaderResourceBinding::VertexStage, ubuf_.get(), kCamSlice),
    });
    srb_->create();

    if (sampler_ && rasterTex_) {
        texSrb_ = makeTexSrb(rasterTex_.get());
        for (auto& kv : tileTex_)
            if (kv.second.tex) kv.second.srb = makeTexSrb(kv.second.tex.get());
    }
}

std::unique_ptr<QRhiShaderResourceBindings> GpuChartView::makeTexSrb(QRhiTexture* t) {
    std::unique_ptr<QRhiShaderResourceBindings> srb(rhi_->newShaderResourceBindings());
    srb->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
            0, QRhiShaderResourceBinding::VertexStage, ubuf_.get(), kCamSlice),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, t, sampler_.get()),
    });
    srb->create();
    return srb;
}

void GpuChartView::initialize(QRhiCommandBuffer*) {
    if (rhi_ != rhi()) {
        const bool lost = (rhi_ != nullptr);
        releaseResources();
        rhi_ = rhi();
        // Record what device we actually got. This is the line that matters when a
        // black-screen report comes in: it proves initialize() ran, on which
        // backend, against which GPU, and — critically — the render-target pixel
        // size (a 0×0 target renders nothing and reads as black).
        if (rhi_) {
            const QRhiDriverInfo di = rhi_->driverInfo();
            const QRhiRenderTarget* rt = renderTarget();
            const QSize rts = rt ? rt->pixelSize() : QSize();
            qCInfo(lcGpu).noquote()
                << QStringLiteral("initialize: backend=%1 device=\"%2\" "
                                  "vendorId=0x%3 deviceType=%4 rt=%5x%6 lost=%7")
                       .arg(QString::fromLatin1(rhi_->backendName()))
                       .arg(QString::fromUtf8(di.deviceName))
                       .arg(QString::number(di.vendorId, 16))
                       .arg(int(di.deviceType))
                       .arg(rts.width()).arg(rts.height())
                       .arg(lost ? 1 : 0);
        } else {
            qCWarning(lcGpu).noquote() << "initialize: rhi() is NULL - no device";
        }
        if (lost && !firstInit_) {
            // The RHI was recreated: retained cells are gone. Tell the owner to
            // re-push them (queued — never re-enter it from inside the render
            // machinery).
            QMetaObject::invokeMethod(this, [this] { emit deviceLost(); },
                                      Qt::QueuedConnection);
        }
    }
    firstInit_ = false;
    if (psTri_)
        return;

    slotStride_ = static_cast<quint32>(rhi_->ubufAligned(kCamSlice));
    ensureUniformCapacity(64);

    QRhiVertexInputLayout layout;
    layout.setBindings({ { kStride } });
    layout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2, 0 },
        { 0, 1, QRhiVertexInputAttribute::Float3, 2 * sizeof(float) },
    });

    const QShader vs = loadShader(QStringLiteral(":/shaders/color.vert.qsb"));
    const QShader fs = loadShader(QStringLiteral(":/shaders/color.frag.qsb"));

    auto makePipeline = [&](QRhiGraphicsPipeline::Topology topo) {
        auto* ps = rhi_->newGraphicsPipeline();
        ps->setTopology(topo);
        ps->setShaderStages({ { QRhiShaderStage::Vertex, vs },
                              { QRhiShaderStage::Fragment, fs } });
        ps->setVertexInputLayout(layout);
        ps->setShaderResourceBindings(srb_.get());
        ps->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        ps->create();
        return ps;
    };
    psTri_.reset(makePipeline(QRhiGraphicsPipeline::Triangles));
    psLine_.reset(makePipeline(QRhiGraphicsPipeline::Lines));

    // Raster tile pipeline: textured quads. Shares the camera uniform (0) and
    // adds a sampled texture (1). A 1x1 placeholder texture's SRB donates the
    // pipeline layout; per-tile SRBs (layout-identical) are bound per draw.
    sampler_.reset(rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                    QRhiSampler::None,
                                    QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    sampler_->create();
    rasterTex_.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
    rasterTex_->create();
    texSrb_ = makeTexSrb(rasterTex_.get());

    const QShader tvs = loadShader(QStringLiteral(":/shaders/tex.vert.qsb"));
    const QShader tfs = loadShader(QStringLiteral(":/shaders/tex.frag.qsb"));
    QRhiVertexInputLayout texLayout;
    texLayout.setBindings({ { 4 * sizeof(float) } });          // x,y,u,v
    texLayout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2, 0 },
        { 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) },
    });
    psTex_.reset(rhi_->newGraphicsPipeline());
    psTex_->setTopology(QRhiGraphicsPipeline::Triangles);
    // Premultiplied-alpha blend so areas of the composited image with no tile
    // (transparent) leave the sea clear untouched instead of writing black.
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable   = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    psTex_->setTargetBlends({ blend });
    psTex_->setShaderStages({ { QRhiShaderStage::Vertex, tvs },
                              { QRhiShaderStage::Fragment, tfs } });
    psTex_->setVertexInputLayout(texLayout);
    psTex_->setShaderResourceBindings(texSrb_.get());
    psTex_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    psTex_->create();
}

void GpuChartView::render(QRhiCommandBuffer* cb) {
    ++telemFrames_;
    // Monotonic (never reset): the fallback watchdog reads this to confirm the
    // device produced output after the widget was shown. Log the first frame and
    // its render-target size — the proof that the RHI path is live, and the value
    // that would reveal a 0×0 (black) swapchain if that is ever the failure mode.
    if (++renderedFrames_ == 1) {
        const QRhiRenderTarget* rt = renderTarget();
        const QSize rts = rt ? rt->pixelSize() : QSize();
        qCInfo(lcGpu).noquote() << QStringLiteral("render: first frame, rt=%1x%2")
                                       .arg(rts.width()).arg(rts.height());
    }
    QRhiResourceUpdateBatch* up = rhi_->nextResourceUpdateBatch();

    // Upload cells whose batches arrived since the last frame: one Immutable
    // buffer set per cell, uploaded once. The update batch takes a copy, so the
    // CPU-side vectors are freed immediately — from here on the GPU buffer is
    // the only copy of the geometry.
    for (auto& kv : cells_) {
        CellEntry& c = kv.second;
        if (!c.pending) continue;
        c.pending = false;
        ++telemSceneUploads_;
        auto makeBuf = [&](std::vector<GpuVertex>& src,
                           std::unique_ptr<QRhiBuffer>& buf, quint32& count) {
            buf.reset();
            count = static_cast<quint32>(src.size());
            if (!count) { src = {}; return; }
            buf.reset(rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                      src.size() * sizeof(GpuVertex)));
            buf->create();
            up->uploadStaticBuffer(buf.get(), src.data());
            src = {};   // free the CPU copy
        };
        makeBuf(c.pendTris,     c.vbufTris,     c.triCount);
        makeBuf(c.pendLines,    c.vbufLines,    c.lineCount);
        makeBuf(c.pendContours, c.vbufContours, c.contourCount);
    }

    // Upload tile textures that arrived since the last frame (once each; the
    // pending image is released after the batch takes its copy).
    for (auto& kv : tileTex_) {
        TileTexEntry& e = kv.second;
        if (e.pending.isNull()) continue;
        const QSize isz = e.pending.size();
        if (!e.tex || e.tex->pixelSize() != isz) {
            e.tex.reset(rhi_->newTexture(QRhiTexture::RGBA8, isz));
            e.tex->create();
            e.srb = makeTexSrb(e.tex.get());
        }
        ++telemTexUploads_;
        up->uploadTexture(e.tex.get(), e.pending);
        e.pending = QImage();
    }

    // Rebuild the tile quad vertex buffer when the drawn set changed. Verts are
    // relative to the camera at rebuild time; every settle reselection also
    // rebuilds, so the origin stays near the viewport and float32 holds.
    if (tileQuadsDirty_) {
        tileQuadsDirty_ = false;
        tileOriginX_ = camX_;
        tileOriginY_ = camY_;
        const quint32 nq = static_cast<quint32>(tileQuads_.size());
        if (nq) {
            std::vector<float> v;
            v.reserve(nq * kTileVertFloats);
            for (const TileQuad& q : tileQuads_) {
                const float x0 = static_cast<float>(q.x0 - tileOriginX_);
                const float x1 = static_cast<float>(q.x1 - tileOriginX_);
                const float y0 = static_cast<float>(q.y0 - tileOriginY_);
                const float y1 = static_cast<float>(q.y1 - tileOriginY_);
                // Image row 0 (v=0) is north = max projected Y (y1).
                const float quad[kTileVertFloats] = {
                    x0, y1, q.u0, q.v0,   // NW
                    x0, y0, q.u0, q.v1,   // SW
                    x1, y1, q.u1, q.v0,   // NE
                    x1, y1, q.u1, q.v0,   // NE
                    x0, y0, q.u0, q.v1,   // SW
                    x1, y0, q.u1, q.v1,   // SE
                };
                v.insert(v.end(), quad, quad + kTileVertFloats);
            }
            if (!tileVbuf_ || tileVbufCap_ < nq) {
                tileVbufCap_ = std::max<quint32>(64, nq * 2);
                tileVbuf_.reset(rhi_->newBuffer(
                    QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                    tileVbufCap_ * kTileVertFloats * sizeof(float)));
                tileVbuf_->create();
            }
            up->updateDynamicBuffer(tileVbuf_.get(), 0,
                                    static_cast<quint32>(v.size() * sizeof(float)),
                                    v.data());
        }
    }

    const QSize sz = renderTarget()->pixelSize();
    const float w = std::max(1, sz.width());
    const float h = std::max(1, sz.height());
    const qreal dpr = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    const float ppmDev = static_cast<float>(ppm_ * dpr);   // device px per metre
    const float sx = 2.0f * ppmDev / w;
    const float sy = 2.0f * ppmDev / h;

    // Viewport in absolute projected metres, for per-cell culling. When the
    // scene is rotated (course-up), the visible region is a rotated rectangle, so
    // expand the axis-aligned cull box to its bounding box (|w·cos|+|h·sin|, …).
    double halfWpx = w / dpr / 2.0, halfHpx = h / dpr / 2.0;
    if (camSin_ != 0.0) {
        const double c = std::abs(camCos_), s = std::abs(camSin_);
        const double hw = halfWpx, hh = halfHpx;
        halfWpx = hw * c + hh * s;
        halfHpx = hw * s + hh * c;
    }
    const double halfWm = halfWpx / ppm_;
    const double halfHm = halfHpx / ppm_;
    const double vx0 = camX_ - halfWm, vx1 = camX_ + halfWm;
    const double vy0 = camY_ - halfHm, vy1 = camY_ + halfHm;

    // Gather the visible entries in draw order, culling whole cells against the
    // viewport, and assign each a per-draw camera uniform slice. Slot 0 is the
    // raster quad's.
    std::vector<const CellEntry*> baseDraw, cellDraw;
    auto gather = [&](const QStringList& keys, std::vector<const CellEntry*>& out) {
        for (const QString& k : keys) {
            const auto it = cells_.find(k);
            if (it == cells_.end()) continue;
            const CellEntry& c = it->second;
            if (c.maxX < vx0 || c.minX > vx1 || c.maxY < vy0 || c.minY > vy1)
                continue;   // entirely outside the viewport
            out.push_back(&c);
        }
    };
    gather(baseList_, baseDraw);
    gather(cellList_, cellDraw);

    ensureUniformCapacity(static_cast<int>(baseDraw.size() + cellDraw.size()) + 1);

    const float rc = static_cast<float>(camCos_);
    const float rs = static_cast<float>(camSin_);
    quint32 nextSlot = 0;
    auto writeSlot = [&](double originX, double originY) -> quint32 {
        // Origin-minus-camera in double, then to float: small for anything near
        // the viewport, so float32 precision holds without any global re-base.
        // Slots 4..7 carry the course-up rotation (cos, sin); same for every draw.
        const float u[8] = { static_cast<float>(camX_ - originX),
                             static_cast<float>(camY_ - originY), sx, sy,
                             rc, rs, 0.0f, 0.0f };
        const quint32 slot = nextSlot++;
        up->updateDynamicBuffer(ubuf_.get(), slot * slotStride_, sizeof(u), u);
        return slot;
    };
    const quint32 tileSlot = writeSlot(tileOriginX_, tileOriginY_);
    std::vector<quint32> baseSlots, cellSlots;
    baseSlots.reserve(baseDraw.size());
    cellSlots.reserve(cellDraw.size());
    for (const CellEntry* c : baseDraw) baseSlots.push_back(writeSlot(c->originX, c->originY));
    for (const CellEntry* c : cellDraw) cellSlots.push_back(writeSlot(c->originX, c->originY));

    telemVertsDrawn_ = 0;

    const QColor sea(204, 224, 242);
    cb->beginPass(renderTarget(), sea, { 1.0f, 0 }, up);
    cb->setViewport(QRhiViewport(0, 0, sz.width(), sz.height()));

    auto drawBucket = [&](const CellEntry* c, quint32 slot, QRhiBuffer* vbuf, quint32 count) {
        if (!count || !vbuf) return;
        const QRhiCommandBuffer::DynamicOffset dynOfs(0, slot * slotStride_);
        cb->setShaderResources(srb_.get(), 1, &dynOfs);
        const QRhiCommandBuffer::VertexInput vin(vbuf, 0);
        cb->setVertexInput(0, 1, &vin);
        cb->draw(count);
        telemVertsDrawn_ += count;
        (void)c;
    };

    // Draw order matches the painter: basemap fills + outlines, then the raster
    // underlay, then chart-cell fills, then chart-cell outlines (+ contours).
    // One pipeline bind per pass; per-draw state is just the uniform slice.
    if (drawFills_) {
        cb->setGraphicsPipeline(psTri_.get());
        for (std::size_t i = 0; i < baseDraw.size(); ++i)
            drawBucket(baseDraw[i], baseSlots[i], baseDraw[i]->vbufTris.get(),
                       baseDraw[i]->triCount);
    }
    {
        cb->setGraphicsPipeline(psLine_.get());
        for (std::size_t i = 0; i < baseDraw.size(); ++i)
            drawBucket(baseDraw[i], baseSlots[i], baseDraw[i]->vbufLines.get(),
                       baseDraw[i]->lineCount);
    }
    if (!tileQuads_.empty() && tileVbuf_) {
        cb->setGraphicsPipeline(psTex_.get());
        const QRhiCommandBuffer::DynamicOffset dynOfs(0, tileSlot * slotStride_);
        for (std::size_t i = 0; i < tileQuads_.size(); ++i) {
            const auto it = tileTex_.find(tileQuads_[i].texId);
            if (it == tileTex_.end() || !it->second.tex) continue;  // not uploaded yet
            cb->setShaderResources(it->second.srb.get(), 1, &dynOfs);
            const QRhiCommandBuffer::VertexInput vin(
                tileVbuf_.get(),
                static_cast<quint32>(i * kTileVertFloats * sizeof(float)));
            cb->setVertexInput(0, 1, &vin);
            cb->draw(6);
            telemVertsDrawn_ += 6;
        }
    }
    if (drawFills_) {
        cb->setGraphicsPipeline(psTri_.get());
        for (std::size_t i = 0; i < cellDraw.size(); ++i)
            drawBucket(cellDraw[i], cellSlots[i], cellDraw[i]->vbufTris.get(),
                       cellDraw[i]->triCount);
    }
    {
        cb->setGraphicsPipeline(psLine_.get());
        for (std::size_t i = 0; i < cellDraw.size(); ++i) {
            drawBucket(cellDraw[i], cellSlots[i], cellDraw[i]->vbufLines.get(),
                       cellDraw[i]->lineCount);
            if (drawContours_)
                drawBucket(cellDraw[i], cellSlots[i], cellDraw[i]->vbufContours.get(),
                           cellDraw[i]->contourCount);
        }
    }
    cb->endPass();
}

void GpuChartView::showEvent(QShowEvent* e) {
    QRhiWidget::showEvent(e);
    // Guarantee a first RHI frame whenever the widget is (re)shown. The owner
    // pushes the camera through setCamera(), but that no-op-guards an unchanged
    // camera — and at startup with no charts the pushed camera equals this
    // widget's default, so nothing there ever schedules the first frame. The
    // widget is then at the mercy of Qt's implicit expose paint landing before
    // the parent composites the translucent overlay drawn on top of the RHI
    // surface; when it loses that race the chart comes up black and, because no
    // camera/scene change ever follows, stays black until an unrelated repaint.
    // Forcing an update() here renders a frame and recomposites the parent, so
    // the sea/chart is always visible from the first shown frame.
    //
    // A child shown while its top-level is still hidden receives this showEvent
    // only when the window finally becomes visible, which is exactly the moment
    // the first frame must be produced. showEvent fires on visibility
    // transitions, not per frame, so the cost is one render on show.
    qCInfo(lcGpu).noquote() << QStringLiteral("showEvent: size=%1x%2 - forcing first frame")
                                   .arg(width()).arg(height());
    update();
}

// ---- interaction (pan/zoom the camera; no geometry rebuild) -----------------

void GpuChartView::mousePressEvent(QMouseEvent* e) {
    lastDrag_ = e->position();
    dragging_ = true;
}

void GpuChartView::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_ || ppm_ <= 0.0) return;
    const QPointF d = e->position() - lastDrag_;
    lastDrag_ = e->position();
    // Drag moves the content with the cursor: shift the camera centre opposite
    // in scene metres. Scene Y is north-up, screen Y is down, so Y is inverted.
    camX_ -= d.x() / ppm_;
    camY_ += d.y() / ppm_;
    update();
}

void GpuChartView::wheelEvent(QWheelEvent* e) {
    if (e->angleDelta().y() == 0) return;
    ppm_ *= (e->angleDelta().y() > 0) ? 1.15 : (1.0 / 1.15);
    update();
}
