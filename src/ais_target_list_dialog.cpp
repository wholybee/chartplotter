#include "ais_target_list_dialog.hpp"
#include "ais_target_store.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QScroller>
#include <QPushButton>
#include <QFrame>
#include <QLabel>
#include <QSizePolicy>
#include <QTimer>
#include <algorithm>
#include <vector>

namespace {
constexpr double kMetresPerNm = 1852.0;
const QString kUnknown  = QStringLiteral("unknown");
const QString kNoNumber = QStringLiteral("—");

QString fmtDistance(const std::optional<double>& rangeMeters) {
    if (!rangeMeters) return kNoNumber;
    return QString::number(*rangeMeters / kMetresPerNm, 'f', 2) + QStringLiteral(" nm");
}

// A cell label that passes mouse events through to its parent row button, so a
// tap anywhere on the row registers as a button click (no per-label handling).
QLabel* makeCell(int fixedWidth, Qt::Alignment align, const QString& color) {
    auto* l = new QLabel;
    l->setAttribute(Qt::WA_TransparentForMouseEvents);
    l->setAlignment(align);
    l->setStyleSheet(QStringLiteral("font-size:14px; padding:0 4px; border:none; color:%1;")
                     .arg(color));
    if (fixedWidth > 0) l->setFixedWidth(fixedWidth);
    return l;
}
} // namespace

AisTargetListDialog::AisTargetListDialog(const AisTargetStore* store, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle(QStringLiteral("AIS Targets"));
    resize(560, 600);

    const theme::MenuPalette& t = theme::menu();
    // Frameless side-menu chrome. Modeless + self-deleting (WA_DeleteOnClose),
    // so the ✕ dismisses via close() (closeOnDismiss) to actually destroy it.
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("AIS Targets"), true);

    countLabel_ = new QLabel;
    countLabel_->setStyleSheet(QStringLiteral("font-size:13px; padding:6px 12px; color:%1;")
                               .arg(t.actionFg));
    panelCol->addWidget(countLabel_);

    // Clickable column headers — tap to sort, tap again to reverse. Widths match
    // the row cells below so the columns line up.
    {
        auto* hdr = new QWidget;
        hdr->setObjectName(QStringLiteral("AisListHdr"));
        hdr->setStyleSheet(QStringLiteral(
            "#AisListHdr{ background:%1; border-bottom:1px solid %2; }")
            .arg(t.headerBg, t.separator));
        auto* hl = new QHBoxLayout(hdr);
        hl->setContentsMargins(8, 4, 8, 4);
        hl->setSpacing(0);

        auto makeHeaderBtn = [&](SortColumn c, int width, bool right) {
            auto* b = new QPushButton(hdr);
            b->setFlat(true);
            b->setCursor(Qt::PointingHandCursor);
            if (width > 0) b->setFixedWidth(width);
            else b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            b->setStyleSheet(QStringLiteral(
                "QPushButton{ border:none; background:transparent; color:%1;"
                " font-size:12px; font-weight:600; padding:0 4px; text-align:%2; }"
                "QPushButton:pressed{ color:%3; }")
                .arg(t.headerFg,
                     right ? QStringLiteral("right") : QStringLiteral("left"),
                     t.accent));
            connect(b, &QPushButton::clicked, this, [this, c] { setSort(c); });
            return b;
        };
        hdrName_ = makeHeaderBtn(SortColumn::Name,     0,   false);
        hdrMmsi_ = makeHeaderBtn(SortColumn::Mmsi,     100, true);
        hdrCall_ = makeHeaderBtn(SortColumn::Call,     100, false);
        hdrDist_ = makeHeaderBtn(SortColumn::Distance, 90,  true);
        hl->addWidget(hdrName_, 1);
        hl->addWidget(hdrMmsi_);
        hl->addWidget(hdrCall_);
        hl->addWidget(hdrDist_);
        panelCol->addWidget(hdr);
        updateHeaderLabels();
    }

    // Scrollable row area. Uses QScrollArea — the same widget the side menu uses
    // via wrapScroll() — so QScroller delivers identical pixel-level drag-to-scroll
    // behaviour: content moves exactly with the finger, no velocity amplification.
    scrollArea_ = new QScrollArea;
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setStyleSheet(QStringLiteral(
        "QScrollArea, QScrollArea > QWidget > QWidget { background:%1; }").arg(t.panelBg));

    rowContainer_ = new QWidget;
    rowLayout_ = new QVBoxLayout(rowContainer_);
    rowLayout_->setContentsMargins(0, 0, 0, 0);
    rowLayout_->setSpacing(0);
    rowLayout_->addStretch(1);          // keeps rows packed to the top
    scrollArea_->setWidget(rowContainer_);

    QScroller::grabGesture(scrollArea_->viewport(), QScroller::LeftMouseButtonGesture);
    panelCol->addWidget(scrollArea_, 1);

    // Coalesce bursts of per-target signals (one per AIS message) into at most
    // one rebuild per 250 ms interval.
    coalesce_ = new QTimer(this);
    coalesce_->setSingleShot(true);
    coalesce_->setInterval(250);
    connect(coalesce_, &QTimer::timeout, this, &AisTargetListDialog::refresh);

    if (store_) {
        connect(store_, &AisTargetStore::targetUpdated, this, &AisTargetListDialog::scheduleRefresh);
        connect(store_, &AisTargetStore::targetExpired, this, &AisTargetListDialog::scheduleRefresh);
    }

    // Distance ticks along with ownship/target motion even when no new AIS
    // messages arrive (e.g. no reports but ownship is underway).
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &AisTargetListDialog::refresh);
    timer_->start();

    refresh();
}

void AisTargetListDialog::scheduleRefresh() {
    if (!coalesce_->isActive()) coalesce_->start();
}

void AisTargetListDialog::setSort(SortColumn c) {
    // Re-tapping the active column reverses it; a new column starts ascending.
    if (sortColumn_ == c) sortAsc_ = !sortAsc_;
    else { sortColumn_ = c; sortAsc_ = true; }
    updateHeaderLabels();
    refresh();
}

void AisTargetListDialog::updateHeaderLabels() {
    auto set = [this](QPushButton* b, SortColumn c, const QString& base) {
        if (!b) return;
        b->setText(sortColumn_ == c
            ? base + (sortAsc_ ? QStringLiteral("  ▲")    // ▲
                               : QStringLiteral("  ▼"))   // ▼
            : base);
    };
    set(hdrName_, SortColumn::Name,     QStringLiteral("Name"));
    set(hdrMmsi_, SortColumn::Mmsi,     QStringLiteral("MMSI"));
    set(hdrCall_, SortColumn::Call,     QStringLiteral("Call sign"));
    set(hdrDist_, SortColumn::Distance, QStringLiteral("Distance"));
}

AisTargetListDialog::Row AisTargetListDialog::makeRow() {
    Row r;
    const theme::MenuPalette& t = theme::menu();
    // Flat button = the tap target. Qt handles click detection; child labels are
    // transparent to the mouse so a tap anywhere on the row hits the button.
    r.btn = new QPushButton(rowContainer_);
    r.btn->setFlat(true);
    r.btn->setMinimumHeight(44);
    r.btn->setStyleSheet(QStringLiteral(
        "QPushButton { text-align:left; border:none; background:%1; color:%2;"
        " border-bottom:1px solid %3; }"
        "QPushButton:pressed { background:%4; }")
        .arg(t.actionBg, t.actionFg, t.separator, t.actionPressed));

    auto* hl = new QHBoxLayout(r.btn);
    hl->setContentsMargins(8, 0, 8, 0);
    hl->setSpacing(0);
    r.name = makeCell(0,   Qt::AlignLeft  | Qt::AlignVCenter, t.actionFg);
    r.name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    r.mmsi = makeCell(100, Qt::AlignRight | Qt::AlignVCenter, t.actionFg);
    r.call = makeCell(100, Qt::AlignLeft  | Qt::AlignVCenter, t.actionFg);
    r.dist = makeCell(90,  Qt::AlignRight | Qt::AlignVCenter, t.actionFg);
    hl->addWidget(r.name, 1);
    hl->addWidget(r.mmsi);
    hl->addWidget(r.call);
    hl->addWidget(r.dist);

    // The row's current MMSI lives in a dynamic property, refreshed each rebuild;
    // the click handler reads it so the dispatch follows the row's current target
    // regardless of sort order.
    connect(r.btn, &QPushButton::clicked, this, [this, btn = r.btn] {
        const quint32 mmsi = btn->property("mmsi").toUInt();
        if (mmsi) emit targetActivated(mmsi);
    });
    return r;
}

void AisTargetListDialog::refresh() {
    if (!store_) return;

    // Snapshot, then sort by the active column/direction. Targets missing the
    // sorted field always sink to the bottom (regardless of direction) so usable
    // contacts stay visible; MMSI is the stable tie-break.
    std::vector<const AisTarget*> targets;
    targets.reserve(store_->targets().size());
    for (const AisTarget& t : store_->targets()) targets.push_back(&t);

    const bool asc = sortAsc_;
    const SortColumn sc = sortColumn_;
    std::sort(targets.begin(), targets.end(),
              [asc, sc](const AisTarget* a, const AisTarget* b) {
        switch (sc) {
        case SortColumn::Name: {
            const bool ha = !a->name.isEmpty(), hb = !b->name.isEmpty();
            if (ha != hb) return ha;                       // named first, always
            if (ha) { const int c = a->name.compare(b->name, Qt::CaseInsensitive);
                      if (c != 0) return asc ? c < 0 : c > 0; }
            return a->mmsi < b->mmsi;
        }
        case SortColumn::Call: {
            const bool ha = !a->callSign.isEmpty(), hb = !b->callSign.isEmpty();
            if (ha != hb) return ha;
            if (ha) { const int c = a->callSign.compare(b->callSign, Qt::CaseInsensitive);
                      if (c != 0) return asc ? c < 0 : c > 0; }
            return a->mmsi < b->mmsi;
        }
        case SortColumn::Distance: {
            const bool ha = a->rangeMeters.has_value(), hb = b->rangeMeters.has_value();
            if (ha != hb) return ha;                       // known range first, always
            if (ha) { const double da = *a->rangeMeters, db = *b->rangeMeters;
                      if (da != db) return asc ? da < db : da > db; }
            return a->mmsi < b->mmsi;
        }
        case SortColumn::Mmsi:
        default:
            if (a->mmsi != b->mmsi) return asc ? a->mmsi < b->mmsi : a->mmsi > b->mmsi;
            return false;
        }
    });

    countLabel_->setText(QStringLiteral("Tracking %1 target%2")
                         .arg(targets.size()).arg(targets.size() == 1 ? "" : "s"));

    // Grow / shrink the reusable row pool to match the target count. Insert new
    // rows before the trailing stretch (the last layout item); destroy surplus
    // rows from the tail. We never touch rows that stay, so the scroll position
    // and any in-progress drag are preserved and nothing flickers.
    const int want = int(targets.size());
    while (int(rows_.size()) < want) {
        Row r = makeRow();
        rowLayout_->insertWidget(rowLayout_->count() - 1, r.btn);
        rows_.push_back(r);
    }
    while (int(rows_.size()) > want) {
        rows_.back().btn->deleteLater();
        rows_.pop_back();
    }

    // Update the surviving rows in place.
    auto setText = [](QLabel* l, const QString& s) { if (l->text() != s) l->setText(s); };
    for (int i = 0; i < want; ++i) {
        const AisTarget* t = targets[i];
        const Row& r = rows_[i];
        setText(r.name, t->name.isEmpty()     ? kUnknown : t->name);
        setText(r.mmsi, QString::number(t->mmsi));
        setText(r.call, t->callSign.isEmpty() ? kUnknown : t->callSign);
        setText(r.dist, fmtDistance(t->rangeMeters));
        r.btn->setProperty("mmsi", t->mmsi);
    }
}
