#include "ais_target_info_window.hpp"
#include "ais_target_store.hpp"
#include "units.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QLabel>
#include <QTimer>
#include <QDateTime>
#include <QStringList>

namespace {
// Shared palette / type face with the nav display window and instruments plugin
// (always dark, regardless of the OS theme, so the panels read as one family).
const QString kText     = QStringLiteral("#e6e9ee");
const QString kCaption  = QStringLiteral("rgba(230,233,238,165)");
const QString kDim      = QStringLiteral("rgba(230,233,238,150)");
const QString kTileBg   = QStringLiteral("rgba(255,255,255,18)");
const QString kHairline = QStringLiteral("rgba(255,255,255,40)");
const QString kGreen    = QStringLiteral("#2e9e44");   // current
const QString kAmber    = QStringLiteral("#ffd24a");   // stale
const QString kRed      = QStringLiteral("#d23b3b");   // lost

constexpr int kTileW   = 106;
constexpr int kTileH   = 60;
constexpr int kCols    = 3;     // metric tiles per row

QString fmtAngle(double v)  { return QString::number(v, 'f', 1); }
QString fmtLat(double v)    { return units::formatLatitude(v); }
QString fmtLon(double v)    { return units::formatLongitude(v); }
QString fmtMeters(double v) { return QString::number(v, 'f', 1) + QStringLiteral(" m"); }
QString fmtNm(double meters){ return QString::number(meters / 1852.0, 'f', 2); }

} // namespace

AisTargetInfoWindow::AisTargetInfoWindow(quint32 mmsi, const AisTargetStore* store,
                                         QWidget* parent)
    : FramelessInfoDialog(parent), mmsi_(mmsi), store_(store) {
    setWindowTitle(QStringLiteral("AIS Target — MMSI %1").arg(mmsi));

    // The frameless translucent window + rounded dark panel come from the base;
    // build our content into its layout.
    auto* col = panelLayout();

    // ---- Header: name + identity subtitle, with a freshness dot and the
    // close button (this window is frameless) at the right.
    auto* header = new QHBoxLayout;
    header->setSpacing(8);

    auto* idCol = new QVBoxLayout;
    idCol->setSpacing(1);
    nameLabel_ = new QLabel(this);
    nameLabel_->setStyleSheet(QStringLiteral("font-size:18px; font-weight:700; color:%1;").arg(kText));
    subtitleLabel_ = new QLabel(this);
    subtitleLabel_->setStyleSheet(QStringLiteral("font-size:12px; color:%1;").arg(kDim));
    subtitleLabel_->setWordWrap(true);
    idCol->addWidget(nameLabel_);
    idCol->addWidget(subtitleLabel_);
    header->addLayout(idCol, 1);

    auto* statusBox = new QWidget(this);
    auto* statusRow = new QHBoxLayout(statusBox);
    statusRow->setContentsMargins(0, 3, 0, 0);
    statusRow->setSpacing(5);
    statusDot_ = new QLabel(statusBox);
    statusDot_->setFixedSize(10, 10);
    statusLabel_ = new QLabel(statusBox);
    statusRow->addWidget(statusDot_);
    statusRow->addWidget(statusLabel_);
    header->addWidget(statusBox, 0, Qt::AlignTop);

    header->addWidget(makeCloseButton(), 0, Qt::AlignTop);

    col->addLayout(header);

    headerSep_ = makeSeparator();
    col->addWidget(headerSep_);

    // ---- Metric tiles (navigation-critical dynamics), centred.
    metricsBox_ = new QWidget(this);
    metricsGrid_ = new QGridLayout(metricsBox_);
    metricsGrid_->setContentsMargins(0, 0, 0, 0);
    metricsGrid_->setHorizontalSpacing(6);
    metricsGrid_->setVerticalSpacing(6);
    col->addWidget(metricsBox_, 0, Qt::AlignHCenter);

    detailsSep_ = makeSeparator();
    col->addWidget(detailsSep_);

    // ---- Details grid (static / voyage / position).
    detailsBox_ = new QWidget(this);
    detailsGrid_ = new QGridLayout(detailsBox_);
    detailsGrid_->setContentsMargins(0, 0, 0, 0);
    detailsGrid_->setHorizontalSpacing(10);
    detailsGrid_->setVerticalSpacing(5);
    col->addWidget(detailsBox_);

    col->addStretch(1);

    if (store_) {
        connect(store_, &AisTargetStore::targetUpdated, this, &AisTargetInfoWindow::onTargetUpdated);
        connect(store_, &AisTargetStore::targetExpired, this, &AisTargetInfoWindow::onTargetExpired);
    }
    // Keep the Age field counting even when no fresh report arrives.
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &AisTargetInfoWindow::refresh);
    timer_->start();

    refresh();
}

void AisTargetInfoWindow::onTargetUpdated(quint32 mmsi) {
    if (mmsi == mmsi_) { lost_ = false; refresh(); }
}

void AisTargetInfoWindow::onTargetExpired(quint32 mmsi) {
    if (mmsi == mmsi_) { lost_ = true; refresh(); }
}

QFrame* AisTargetInfoWindow::makeSeparator() {
    auto* s = new QFrame(this);
    s->setFrameShape(QFrame::HLine);
    s->setStyleSheet(QStringLiteral("color:%1;").arg(kHairline));
    return s;
}

void AisTargetInfoWindow::clearGrid(QGridLayout* grid) {
    QLayoutItem* item = nullptr;
    while ((item = grid->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
}

void AisTargetInfoWindow::rebuildMetrics(const QVector<Metric>& metrics) {
    clearGrid(metricsGrid_);
    metricValues_.clear();

    for (int i = 0; i < metrics.size(); ++i) {
        const Metric& m = metrics[i];
        auto* tile = new QFrame(metricsBox_);
        tile->setFixedSize(kTileW, kTileH);
        tile->setStyleSheet(QStringLiteral(
            "QFrame{ background:%1; border-radius:6px; }").arg(kTileBg));

        auto* v = new QVBoxLayout(tile);
        v->setContentsMargins(4, 5, 4, 5);
        v->setSpacing(0);

        auto* cap = new QLabel(m.caption, tile);
        cap->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        cap->setStyleSheet(QStringLiteral("font-size:11px; font-weight:600; color:%1;").arg(kCaption));

        auto* val = new QLabel(m.value, tile);
        val->setAlignment(Qt::AlignCenter);
        val->setStyleSheet(QStringLiteral("font-size:19px; font-weight:700; color:%1;").arg(kText));

        v->addWidget(cap);
        v->addStretch(1);
        v->addWidget(val);
        if (!m.unit.isEmpty()) {
            auto* u = new QLabel(m.unit, tile);
            u->setAlignment(Qt::AlignHCenter);
            u->setStyleSheet(QStringLiteral("font-size:10px; color:%1;").arg(kDim));
            v->addWidget(u);
        } else {
            v->addStretch(1);
        }

        metricsGrid_->addWidget(tile, i / kCols, i % kCols);
        metricValues_.insert(m.caption, val);
    }
}

void AisTargetInfoWindow::rebuildDetails(const QVector<Detail>& details) {
    clearGrid(detailsGrid_);
    detailValues_.clear();

    auto makeCaption = [this](const QString& text) {
        auto* l = new QLabel(text, detailsBox_);
        l->setStyleSheet(QStringLiteral("font-size:12px; color:%1;").arg(kDim));
        l->setMinimumWidth(58);
        l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        return l;
    };
    auto makeValue = [this](const QString& text, bool wrap) {
        auto* l = new QLabel(text, detailsBox_);
        l->setStyleSheet(QStringLiteral("font-size:13px; font-weight:600; color:%1;").arg(kText));
        l->setWordWrap(wrap);
        l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        return l;
    };

    int row = 0, half = 0;   // half: 0 = left pair (cols 0/1), 1 = right pair (cols 2/3)
    for (const Detail& d : details) {
        if (d.wide) {
            if (half == 1) { ++row; half = 0; }          // finish the open row first
            detailsGrid_->addWidget(makeCaption(d.caption), row, 0);
            auto* v = makeValue(d.value, true);
            detailsGrid_->addWidget(v, row, 1, 1, 3);
            detailValues_.insert(d.caption, v);
            ++row; half = 0;
        } else {
            const int base = (half == 0) ? 0 : 2;
            detailsGrid_->addWidget(makeCaption(d.caption), row, base);
            auto* v = makeValue(d.value, false);
            detailsGrid_->addWidget(v, row, base + 1);
            detailValues_.insert(d.caption, v);
            if (half == 0) half = 1;
            else { half = 0; ++row; }
        }
    }
    detailsGrid_->setColumnStretch(1, 1);
    detailsGrid_->setColumnStretch(3, 1);
}

void AisTargetInfoWindow::refresh() {
    // If the target is still tracked, read its current state; otherwise we keep
    // showing the last known fields (Status: Lost).
    static const AisTarget kEmpty;
    const AisTarget* live = store_ ? store_->target(mmsi_) : nullptr;
    const AisTarget& t = live ? *live : kEmpty;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const bool isLost = lost_ || !live;

    // ---- Header.
    const QString name = t.name.trimmed();
    QStringList sub;
    if (!name.isEmpty()) {
        nameLabel_->setText(name);
        sub << QStringLiteral("MMSI %1").arg(mmsi_);
    } else {
        nameLabel_->setText(QStringLiteral("MMSI %1").arg(mmsi_));
    }
    // Class / kind. For a distress beacon, name the specific device (SART vs MOB
    // vs EPIRB) from the MMSI rather than the generic class label.
    QString clsLabel = aisClassName(t.cls);
    if (t.cls == AisClass::Sart) {
        const QString kind = aisDistressKind(mmsi_);
        if (!kind.isEmpty()) clsLabel = kind;
    }
    if (!clsLabel.isEmpty()) sub << clsLabel;
    // Type descriptor: AtoN aid type, otherwise the ship & cargo type.
    if (t.cls == AisClass::AtoN) { if (t.atonType) sub << aisAtonTypeName(*t.atonType); }
    else if (t.shipType)         sub << aisShipTypeName(*t.shipType);
    subtitleLabel_->setText(sub.join(QStringLiteral("  ·  ")));
    subtitleLabel_->setVisible(!sub.isEmpty());

    // ---- Freshness dot + label.
    const AisFreshness fr = isLost ? AisFreshness::Lost : t.freshness;
    QString dotCol = kGreen, statusText = QStringLiteral("Current");
    switch (fr) {
        case AisFreshness::Current: dotCol = kGreen; statusText = QStringLiteral("Current"); break;
        case AisFreshness::Stale:   dotCol = kAmber; statusText = QStringLiteral("Stale");   break;
        case AisFreshness::Lost:    dotCol = kRed;   statusText = QStringLiteral("Lost");    break;
    }
    statusDot_->setStyleSheet(QStringLiteral("background:%1; border-radius:5px;").arg(dotCol));
    statusLabel_->setText(statusText);
    statusLabel_->setStyleSheet(QStringLiteral("font-size:12px; font-weight:600; color:%1;").arg(dotCol));

    // ---- Metric tiles (navigation-critical dynamics).
    QVector<Metric> metrics;
    if (t.sogKnots)       metrics.push_back({QStringLiteral("SOG"),  QString::number(*t.sogKnots, 'f', 1), QStringLiteral("kn")});
    if (t.cogDegTrue)     metrics.push_back({QStringLiteral("COG"),  fmtAngle(*t.cogDegTrue),               QStringLiteral("°T")});
    if (t.headingDegTrue) metrics.push_back({QStringLiteral("HDG"),  fmtAngle(*t.headingDegTrue),           QStringLiteral("°T")});
    if (t.altitudeMeters) metrics.push_back({QStringLiteral("ALT"),  QString::number(*t.altitudeMeters, 'f', 0), QStringLiteral("m")});
    if (t.rangeMeters)    metrics.push_back({QStringLiteral("DIST"), fmtNm(*t.rangeMeters),                 QStringLiteral("nm")});
    if (t.cpaMeters)      metrics.push_back({QStringLiteral("CPA"),  fmtNm(*t.cpaMeters),                   QStringLiteral("nm")});
    if (t.tcpaSeconds)    metrics.push_back({QStringLiteral("TCPA"), aisFormatTcpa(*t.tcpaSeconds),         QString()});

    // ---- Details (static / voyage / position / provenance).
    QVector<Detail> details;
    auto addDetail = [&](const QString& c, const QString& v, bool wide = false) {
        details.push_back({c, v, wide});
    };
    if (!t.callSign.isEmpty()) addDetail(QStringLiteral("Call sign"), t.callSign);
    if (t.imoNumber)           addDetail(QStringLiteral("IMO"),       QString::number(*t.imoNumber));
    if (t.dimensions.known()) {
        addDetail(QStringLiteral("Length"), fmtMeters(t.dimensions.lengthMeters()));
        addDetail(QStringLiteral("Beam"),   fmtMeters(t.dimensions.beamMeters()));
    }
    if (t.draughtMeters) addDetail(QStringLiteral("Draught"), fmtMeters(*t.draughtMeters));
    if (t.rotDegPerMin)
        addDetail(QStringLiteral("ROT"),
                  QString::number(*t.rotDegPerMin, 'f', 1) + QStringLiteral(" °/min"));
    if (t.navStatus != AisNavStatus::Undefined && t.cls == AisClass::A)
        addDetail(QStringLiteral("Nav status"), aisNavStatusName(int(t.navStatus)), true);
    if (t.cls == AisClass::AtoN) {
        addDetail(QStringLiteral("Aid"),
                  t.atonVirtual ? QStringLiteral("Virtual") : QStringLiteral("Real"));
        addDetail(QStringLiteral("Position"),
                  t.atonOffPosition ? QStringLiteral("Off position")
                                    : QStringLiteral("On position"));
    }
    if (!t.destination.isEmpty())
        addDetail(QStringLiteral("Destination"), t.destination, true);
    if (t.latitudeDeg)  addDetail(QStringLiteral("Lat"), fmtLat(*t.latitudeDeg));
    if (t.longitudeDeg) addDetail(QStringLiteral("Lon"), fmtLon(*t.longitudeDeg));
    if (!t.source.isEmpty()) addDetail(QStringLiteral("Source"), t.source);

    // Age (relative to last update). When the target is gone from the store we
    // have no live timestamp, so the field is omitted (Status already says Lost).
    const double age = (live && t.lastUpdateUtc.isValid())
        ? t.lastUpdateUtc.msecsTo(now) / 1000.0
        : (live ? 0.0 : -1.0);
    if (age >= 0.0)
        addDetail(QStringLiteral("Age"), QString::number(age, 'f', 0) + QStringLiteral(" s"));

    // ---- Rebuild the widget tree only when the field set changes; otherwise
    // update the live values in place so a refresh never flickers or resizes.
    QString sig;
    for (const Metric& m : metrics) sig += m.caption + QLatin1Char('|');
    sig += QLatin1Char('#');
    for (const Detail& d : details) sig += d.caption + (d.wide ? QStringLiteral("W") : QString()) + QLatin1Char('|');

    if (sig != lastSig_) {
        rebuildMetrics(metrics);
        rebuildDetails(details);
        const bool hasMetrics = !metrics.isEmpty();
        const bool hasDetails = !details.isEmpty();
        headerSep_->setVisible(hasMetrics || hasDetails);
        metricsBox_->setVisible(hasMetrics);
        detailsSep_->setVisible(hasMetrics && hasDetails);
        detailsBox_->setVisible(hasDetails);
        lastSig_ = sig;
        adjustSize();
    } else {
        for (const Metric& m : metrics)
            if (QLabel* l = metricValues_.value(m.caption)) l->setText(m.value);
        for (const Detail& d : details)
            if (QLabel* l = detailValues_.value(d.caption)) l->setText(d.value);
    }
}
