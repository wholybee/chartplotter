// src/gpu_chart_view.cpp
#include "gpu_chart_view.hpp"

#include <rhi/qrhi.h>
#include <QColor>
#include <QFile>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QSize>
#include <algorithm>

namespace {

QShader loadShader(const QString& name) {
    QFile f(name);
    if (f.open(QIODevice::ReadOnly))
        return QShader::fromSerialized(f.readAll());
    return QShader();
}

constexpr int kStride = 5 * sizeof(float);   // x,y,r,g,b

} // namespace

GpuChartView::GpuChartView(QWidget* parent) : QRhiWidget(parent) {
#if defined(Q_OS_WIN)
    setApi(QRhiWidget::Api::Direct3D11);
#endif
}

GpuChartView::~GpuChartView() = default;

void GpuChartView::setScene(std::vector<GpuVertex> baseTris, std::vector<GpuVertex> baseLines,
                            std::vector<GpuVertex> cellTris, std::vector<GpuVertex> cellLines,
                            double centerX, double centerY, double ppm) {
    baseData_     = std::move(baseTris);
    baseLineData_ = std::move(baseLines);
    triData_      = std::move(cellTris);
    lineData_     = std::move(cellLines);
    baseCount_     = static_cast<quint32>(baseData_.size());
    baseLineCount_ = static_cast<quint32>(baseLineData_.size());
    triCount_      = static_cast<quint32>(triData_.size());
    lineCount_     = static_cast<quint32>(lineData_.size());
    camX_ = centerX;
    camY_ = centerY;
    ppm_  = (ppm > 0.0) ? ppm : 1.0;
    sceneDirty_ = true;
    update();
}

void GpuChartView::setCamera(double centerX, double centerY, double ppm) {
    camX_ = centerX;
    camY_ = centerY;
    ppm_  = (ppm > 0.0) ? ppm : 1.0;
    update();   // uniform-only refresh; no geometry rebuild
}

void GpuChartView::setRasterLayer(const QImage& img, double halfW, double halfH) {
    if (img.isNull() || halfW <= 0.0 || halfH <= 0.0) {
        hasRaster_ = false;
        rasterImg_ = QImage();
        rasterDirty_ = true;
        update();
        return;
    }
    rasterImg_   = img.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    rasterHalfW_ = halfW;
    rasterHalfH_ = halfH;
    hasRaster_   = true;
    rasterDirty_ = true;
    update();
}

bool GpuChartView::isAvailable() {
    // Cache the probe: -1 unknown, 0 no, 1 yes. RHI creation is not free, and the
    // answer can't change during a run.
    static int cached = -1;
    if (cached >= 0)
        return cached == 1;
    bool ok = false;
#if defined(Q_OS_WIN)
    QRhiD3D11InitParams params;
    if (QRhi* rhi = QRhi::create(QRhi::D3D11, &params)) {
        delete rhi;
        ok = true;
    }
#else
    QRhiNullInitParams params;
    if (QRhi* rhi = QRhi::create(QRhi::Null, &params)) {
        delete rhi;
        ok = true;
    }
#endif
    cached = ok ? 1 : 0;
    return ok;
}

void GpuChartView::releaseResources() {
    psTri_.reset();
    psLine_.reset();
    srb_.reset();
    ubuf_.reset();
    vbufBase_.reset();
    vbufBaseLines_.reset();
    vbufTris_.reset();
    vbufLines_.reset();
    psTex_.reset();
    texSrb_.reset();
    sampler_.reset();
    rasterTex_.reset();
    rasterVbuf_.reset();
    sceneDirty_  = true;   // buffers must be rebuilt against the new RHI
    rasterDirty_ = true;
}

void GpuChartView::initialize(QRhiCommandBuffer*) {
    if (rhi_ != rhi()) {
        releaseResources();
        rhi_ = rhi();
    }
    if (psTri_)
        return;

    ubuf_.reset(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                4 * sizeof(float)));
    ubuf_->create();

    srb_.reset(rhi_->newShaderResourceBindings());
    srb_->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage, ubuf_.get()),
    });
    srb_->create();

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

    // Raster underlay pipeline: a textured quad. Shares the camera uniform (0) and
    // adds a sampled texture (1). A 1x1 placeholder texture lets the SRB + pipeline
    // be built now; the real image is uploaded in render() when it arrives.
    sampler_.reset(rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                    QRhiSampler::None,
                                    QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    sampler_->create();
    rasterTex_.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
    rasterTex_->create();
    texSrb_.reset(rhi_->newShaderResourceBindings());
    texSrb_->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage, ubuf_.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, rasterTex_.get(), sampler_.get()),
    });
    texSrb_->create();

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
    rasterDirty_ = true;
}

void GpuChartView::render(QRhiCommandBuffer* cb) {
    QRhiResourceUpdateBatch* up = rhi_->nextResourceUpdateBatch();

    // (Re)create and upload the vertex buffers when the scene changed.
    if (sceneDirty_) {
        vbufBase_.reset();
        vbufBaseLines_.reset();
        vbufTris_.reset();
        vbufLines_.reset();
        if (baseCount_) {
            vbufBase_.reset(rhi_->newBuffer(QRhiBuffer::Immutable,
                                            QRhiBuffer::VertexBuffer,
                                            baseData_.size() * sizeof(GpuVertex)));
            vbufBase_->create();
            up->uploadStaticBuffer(vbufBase_.get(), baseData_.data());
        }
        if (baseLineCount_) {
            vbufBaseLines_.reset(rhi_->newBuffer(QRhiBuffer::Immutable,
                                                 QRhiBuffer::VertexBuffer,
                                                 baseLineData_.size() * sizeof(GpuVertex)));
            vbufBaseLines_->create();
            up->uploadStaticBuffer(vbufBaseLines_.get(), baseLineData_.data());
        }
        if (triCount_) {
            vbufTris_.reset(rhi_->newBuffer(QRhiBuffer::Immutable,
                                            QRhiBuffer::VertexBuffer,
                                            triData_.size() * sizeof(GpuVertex)));
            vbufTris_->create();
            up->uploadStaticBuffer(vbufTris_.get(), triData_.data());
        }
        if (lineCount_) {
            vbufLines_.reset(rhi_->newBuffer(QRhiBuffer::Immutable,
                                             QRhiBuffer::VertexBuffer,
                                             lineData_.size() * sizeof(GpuVertex)));
            vbufLines_->create();
            up->uploadStaticBuffer(vbufLines_.get(), lineData_.data());
        }
        sceneDirty_ = false;
    }

    // (Re)create the raster texture + its quad when the composited image changed.
    if (rasterDirty_) {
        if (hasRaster_ && !rasterImg_.isNull()) {
            const QSize isz = rasterImg_.size();
            if (!rasterTex_ || rasterTex_->pixelSize() != isz) {
                rasterTex_.reset(rhi_->newTexture(QRhiTexture::RGBA8, isz));
                rasterTex_->create();
                texSrb_->setBindings({
                    QRhiShaderResourceBinding::uniformBuffer(
                        0, QRhiShaderResourceBinding::VertexStage, ubuf_.get()),
                    QRhiShaderResourceBinding::sampledTexture(
                        1, QRhiShaderResourceBinding::FragmentStage,
                        rasterTex_.get(), sampler_.get()),
                });
                texSrb_->create();
            }
            up->uploadTexture(rasterTex_.get(), rasterImg_);
            // Quad covering the scene rect centred on the origin (±half extents),
            // with UVs so the image's top row maps to north (max scene Y).
            const float hw = static_cast<float>(rasterHalfW_);
            const float hh = static_cast<float>(rasterHalfH_);
            const float quad[6 * 4] = {
                -hw,  hh, 0.0f, 0.0f,   // NW
                -hw, -hh, 0.0f, 1.0f,   // SW
                 hw,  hh, 1.0f, 0.0f,   // NE
                 hw,  hh, 1.0f, 0.0f,   // NE
                -hw, -hh, 0.0f, 1.0f,   // SW
                 hw, -hh, 1.0f, 1.0f,   // SE
            };
            rasterVbuf_.reset(rhi_->newBuffer(QRhiBuffer::Immutable,
                                              QRhiBuffer::VertexBuffer, sizeof(quad)));
            rasterVbuf_->create();
            up->uploadStaticBuffer(rasterVbuf_.get(), quad);
        } else {
            rasterVbuf_.reset();
        }
        rasterDirty_ = false;
    }

    // Camera uniform: centre (scene metres) + NDC units per metre (zoom + aspect).
    const QSize sz = renderTarget()->pixelSize();
    const float w = std::max(1, sz.width());
    const float h = std::max(1, sz.height());
    const qreal dpr = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;
    const float ppmDev = static_cast<float>(ppm_ * dpr);   // device px per metre
    const float cam[4] = {
        static_cast<float>(camX_), static_cast<float>(camY_),
        2.0f * ppmDev / w, 2.0f * ppmDev / h,
    };
    up->updateDynamicBuffer(ubuf_.get(), 0, sizeof(cam), cam);

    const QColor sea(204, 224, 242);
    cb->beginPass(renderTarget(), sea, { 1.0f, 0 }, up);
    cb->setViewport(QRhiViewport(0, 0, sz.width(), sz.height()));

    // Draw order matches the painter: basemap fills + outlines, then the raster
    // underlay, then chart-cell fills, then chart-cell outlines.
    if (baseCount_ && vbufBase_) {
        cb->setGraphicsPipeline(psTri_.get());
        cb->setShaderResources();
        const QRhiCommandBuffer::VertexInput vin(vbufBase_.get(), 0);
        cb->setVertexInput(0, 1, &vin);
        cb->draw(baseCount_);
    }
    if (baseLineCount_ && vbufBaseLines_) {
        cb->setGraphicsPipeline(psLine_.get());
        cb->setShaderResources();
        const QRhiCommandBuffer::VertexInput vin(vbufBaseLines_.get(), 0);
        cb->setVertexInput(0, 1, &vin);
        cb->draw(baseLineCount_);
    }
    if (hasRaster_ && rasterVbuf_ && rasterTex_) {
        cb->setGraphicsPipeline(psTex_.get());
        cb->setShaderResources(texSrb_.get());
        const QRhiCommandBuffer::VertexInput vin(rasterVbuf_.get(), 0);
        cb->setVertexInput(0, 1, &vin);
        cb->draw(6);
    }
    if (triCount_ && vbufTris_) {
        cb->setGraphicsPipeline(psTri_.get());
        cb->setShaderResources();
        const QRhiCommandBuffer::VertexInput vin(vbufTris_.get(), 0);
        cb->setVertexInput(0, 1, &vin);
        cb->draw(triCount_);
    }
    if (lineCount_ && vbufLines_) {
        cb->setGraphicsPipeline(psLine_.get());
        cb->setShaderResources();
        const QRhiCommandBuffer::VertexInput vin(vbufLines_.get(), 0);
        cb->setVertexInput(0, 1, &vin);
        cb->draw(lineCount_);
    }
    cb->endPass();
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
