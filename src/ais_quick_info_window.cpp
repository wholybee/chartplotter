#include "ais_quick_info_window.hpp"
#include "ais_target_store.hpp"
#include "ais_alarm.hpp"
#include "theme.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

AisQuickInfoWindow::AisQuickInfoWindow(quint32 mmsi, const AisTargetStore* store,
                                       AisAlarm* alarm, QWidget* parent)
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint),
      mmsi_(mmsi), store_(store), alarm_(alarm) {
    // Show without grabbing focus so the chart keeps receiving the clicks/pans
    // that dismiss this popup, and delete on close so the caller's QPointer clears.
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_DeleteOnClose, true);
    setFrameShape(QFrame::StyledPanel);
    setStyleSheet(QStringLiteral(
        "AisQuickInfoWindow { background:%1; border:1px solid %2; border-radius:6px; }")
        .arg(theme::menu().panelBg, theme::menu().panelBorder));

    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(12, 8, 12, 8);
    col->setSpacing(2);

    // Title row: name on the left, a close (X) button pinned to the top-right —
    // the popup's only explicit dismiss (chart interaction also closes it).
    auto* titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(8);
    titleLabel_ = new QLabel(this);
    titleLabel_->setStyleSheet(QStringLiteral("font-size:14px; font-weight:600;"));
    titleRow->addWidget(titleLabel_, 1);

    auto* closeBtn = new QPushButton(QString(QChar(0x2715)), this);   // U+2715 MULTIPLICATION X
    closeBtn->setFixedSize(26, 26);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setStyleSheet(QStringLiteral(
        "QPushButton{ border:none; background:transparent; color:%1;"
        " font-size:15px; font-weight:600; }"
        "QPushButton:pressed{ color:%2; }")
        .arg(theme::textMuted(), theme::menu().actionFg));
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);
    titleRow->addWidget(closeBtn, 0, Qt::AlignTop);
    col->addLayout(titleRow);

    cogLabel_ = new QLabel(this);
    sogLabel_ = new QLabel(this);
    distLabel_ = new QLabel(this);
    cpaLabel_ = new QLabel(this);
    tcpaLabel_ = new QLabel(this);
    for (QLabel* l : {cogLabel_, sogLabel_, distLabel_, cpaLabel_, tcpaLabel_})
        l->setStyleSheet(QStringLiteral("font-size:13px;"));
    col->addWidget(cogLabel_);
    col->addWidget(sogLabel_);
    col->addWidget(distLabel_);
    col->addWidget(cpaLabel_);
    col->addWidget(tcpaLabel_);

    // Mute/acknowledge: only shown for a dangerous target while the alarm is on.
    // Amber to read as an alarm control, distinct from the info text above.
    muteBtn_ = new QPushButton(this);
    muteBtn_->setStyleSheet(QStringLiteral(
        "QPushButton{ min-height:34px; margin-top:6px; font-weight:600; color:white;"
        " background:#d9822b; border-radius:4px; padding:0 12px; }"
        "QPushButton:disabled{ background:#7a5a33; color:#e0e0e0; }"));
    muteBtn_->setVisible(false);
    col->addWidget(muteBtn_);
    connect(muteBtn_, &QPushButton::clicked, this, [this] {
        if (alarm_) alarm_->acknowledge(mmsi_);
        refresh();
    });

    if (store_) {
        connect(store_, &AisTargetStore::targetUpdated, this, [this](quint32 m) {
            if (m == mmsi_) refresh();
        });
        // The target aged out — the quick look has nothing left to show.
        connect(store_, &AisTargetStore::targetExpired, this, [this](quint32 m) {
            if (m == mmsi_) close();
        });
    }
    // Danger/mute state can change without a store update (auto-unmute, another
    // popup acknowledging); refresh the button when it does.
    if (alarm_)
        connect(alarm_, &AisAlarm::stateChanged, this, &AisQuickInfoWindow::refresh);
    refresh();
}

void AisQuickInfoWindow::refresh() {
    const AisTarget* t = store_ ? store_->target(mmsi_) : nullptr;

    const QString title = (t && !t->name.isEmpty())
        ? t->name
        : QStringLiteral("MMSI %1").arg(mmsi_);
    titleLabel_->setText(title);

    cogLabel_->setText(QStringLiteral("COG: %1").arg(
        (t && t->cogDegTrue)
            ? QString::number(*t->cogDegTrue, 'f', 1) + QStringLiteral("°")
            : QStringLiteral("—")));
    sogLabel_->setText(QStringLiteral("SOG: %1").arg(
        (t && t->sogKnots)
            ? QString::number(*t->sogKnots, 'f', 1) + QStringLiteral(" kn")
            : QStringLiteral("—")));

    // Distance to ownship is known whenever both positions are.
    const bool haveDist = t && t->rangeMeters;
    distLabel_->setVisible(haveDist);
    if (haveDist)
        distLabel_->setText(QStringLiteral("Distance: %1 nm").arg(
            QString::number(*t->rangeMeters / 1852.0, 'f', 2)));

    // CPA/TCPA only exist once the collision component has a fix on both vessels;
    // show them only when available so the popup stays a quick glance otherwise.
    const bool haveCpa = t && t->cpaMeters && t->tcpaSeconds;
    cpaLabel_->setVisible(haveCpa);
    tcpaLabel_->setVisible(haveCpa);
    if (haveCpa) {
        cpaLabel_->setText(QStringLiteral("CPA: %1 nm").arg(
            QString::number(*t->cpaMeters / 1852.0, 'f', 2)));
        tcpaLabel_->setText(QStringLiteral("TCPA: %1").arg(
            aisFormatTcpa(*t->tcpaSeconds)));
    }

    // Mute button: present only while this target is a dangerous one that the
    // audible alarm would sound on. Once muted it stays visible (disabled,
    // reading "Muted") so the user can see the alarm is acknowledged.
    const bool showMute = t && alarm_ && alarm_->soundEnabled() && alarm_->isDangerous(mmsi_);
    muteBtn_->setVisible(showMute);
    if (showMute) {
        const bool muted = alarm_->isAcknowledged(mmsi_);
        muteBtn_->setText(muted ? QStringLiteral("Muted") : QStringLiteral("Mute alarm"));
        muteBtn_->setEnabled(!muted);
    }

    adjustSize();
}
