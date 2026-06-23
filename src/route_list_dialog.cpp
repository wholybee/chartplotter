#include "route_list_dialog.hpp"
#include "route_store.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QScroller>
#include <QPushButton>
#include <QCheckBox>
#include <QFrame>
#include <QLabel>
#include <QSizePolicy>
#include <QMessageBox>

namespace {
QLabel* makeCell(int fixedWidth, Qt::Alignment align) {
    auto* l = new QLabel;
    l->setAttribute(Qt::WA_TransparentForMouseEvents);
    l->setAlignment(align);
    l->setStyleSheet(QStringLiteral("font-size:14px; padding:0 4px; border:none; color:%1;")
                     .arg(theme::menu().actionFg));
    if (fixedWidth > 0) l->setFixedWidth(fixedWidth);
    return l;
}
}  // namespace

RouteListDialog::RouteListDialog(RouteStore* store, bool pickMode, QWidget* parent)
    : QDialog(parent), store_(store), pickMode_(pickMode) {
    const QString titleText = pickMode ? QStringLiteral("Select Route")
                                       : QStringLiteral("Routes");
    setWindowTitle(titleText);
    resize(520, 600);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, titleText);

    countLabel_ = new QLabel;
    countLabel_->setStyleSheet(QStringLiteral("font-size:13px; padding:6px 12px; color:%1;")
                               .arg(t.actionFg));
    panelCol->addWidget(countLabel_);

    {   // static column header
        auto* hdr = new QWidget;
        hdr->setObjectName(QStringLiteral("RouteListHdr"));
        hdr->setStyleSheet(QStringLiteral(
            "#RouteListHdr{ background:%1; border-bottom:1px solid %2; }")
            .arg(t.headerBg, t.separator));
        auto* hl = new QHBoxLayout(hdr);
        hl->setContentsMargins(8, 4, 8, 4);
        hl->setSpacing(0);
        auto headerCell = [&](const QString& text, int width, Qt::Alignment align) {
            auto* l = new QLabel(text, hdr);
            l->setStyleSheet(QStringLiteral(
                "font-size:12px; font-weight:600; color:%1; padding:0 4px;").arg(t.headerFg));
            if (width > 0) l->setFixedWidth(width);
            else l->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            l->setAlignment(align);
            return l;
        };
        hl->addWidget(headerCell(QStringLiteral("Name"),    0,  Qt::AlignLeft  | Qt::AlignVCenter), 1);
        hl->addWidget(headerCell(QStringLiteral("Points"),  70, Qt::AlignRight | Qt::AlignVCenter));
        hl->addWidget(headerCell(QStringLiteral("Visible"), 64, Qt::AlignCenter));
        panelCol->addWidget(hdr);
    }

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
    rowLayout_->addStretch(1);
    scrollArea_->setWidget(rowContainer_);
    QScroller::grabGesture(scrollArea_->viewport(), QScroller::LeftMouseButtonGesture);
    panelCol->addWidget(scrollArea_, 1);

    // Shared button styling: outlined for the secondary actions, accent-filled
    // for the default Close/Cancel.
    auto outlined = [](QPushButton* b) { dialogchrome::styleOutlinedButton(b); };
    auto accent   = [](QPushButton* b) { dialogchrome::styleAccentButton(b); };

    auto* btnBar = new QWidget;
    auto* btnRow = new QHBoxLayout(btnBar);
    btnRow->setContentsMargins(12, 8, 12, 12);
    btnRow->setSpacing(8);
    deleteBtn_ = new QPushButton(QStringLiteral("Delete"), btnBar);
    deleteBtn_->setEnabled(false);
    deleteBtn_->setVisible(!pickMode_);
    outlined(deleteBtn_);
    connect(deleteBtn_, &QPushButton::clicked, this, &RouteListDialog::deleteSelected);
    btnRow->addWidget(deleteBtn_);
    propsBtn_ = new QPushButton(QStringLiteral("Properties"), btnBar);
    propsBtn_->setEnabled(false);
    propsBtn_->setVisible(!pickMode_);
    outlined(propsBtn_);
    connect(propsBtn_, &QPushButton::clicked, this, [this] {
        if (selectedId_ >= 0) emit propertiesRequested(selectedId_);
    });
    btnRow->addWidget(propsBtn_);
    editBtn_ = new QPushButton(QStringLiteral("Edit on Chart"), btnBar);
    editBtn_->setEnabled(false);
    editBtn_->setVisible(!pickMode_);
    outlined(editBtn_);
    connect(editBtn_, &QPushButton::clicked, this, [this] {
        if (selectedId_ >= 0) { emit editRequested(selectedId_); accept(); }
    });
    btnRow->addWidget(editBtn_);
    btnRow->addStretch(1);
    newBtn_ = new QPushButton(QStringLiteral("New"), btnBar);
    newBtn_->setVisible(!pickMode_);
    outlined(newBtn_);
    connect(newBtn_, &QPushButton::clicked, this, [this] {
        emit newRouteRequested();
        accept();   // close the list — the user is now drawing
    });
    btnRow->addWidget(newBtn_);
    auto* closeBtn = new QPushButton(pickMode_ ? QStringLiteral("Cancel")
                                               : QStringLiteral("Close"), btnBar);
    closeBtn->setDefault(true);
    accent(closeBtn);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(closeBtn);
    panelCol->addWidget(btnBar);

    if (store_) connect(store_, &RouteStore::routesChanged, this, &RouteListDialog::refresh);
    refresh();
}

RouteListDialog::Row RouteListDialog::makeRow() {
    Row r;
    r.btn = new QPushButton(rowContainer_);
    r.btn->setFlat(true);
    r.btn->setMinimumHeight(44);
    r.btn->setCheckable(true);
    const theme::MenuPalette& t = theme::menu();
    r.btn->setStyleSheet(QStringLiteral(
        "QPushButton { text-align:left; border:none; background:%1; color:%2;"
        " border-bottom:1px solid %3; }"
        "QPushButton:checked { background:%4; color:%5; }")
        .arg(t.actionBg, t.actionFg, t.separator, t.accent, t.titleFg));
    auto* hl = new QHBoxLayout(r.btn);
    hl->setContentsMargins(8, 0, 8, 0);
    hl->setSpacing(0);
    r.name = makeCell(0,  Qt::AlignLeft  | Qt::AlignVCenter);
    r.name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    r.meta = makeCell(70, Qt::AlignRight | Qt::AlignVCenter);
    hl->addWidget(r.name, 1);
    hl->addWidget(r.meta);

    // Visible checkbox: kept clickable (not transparent) so it toggles
    // independently of selecting the row.
    r.vis = new QCheckBox(r.btn);
    r.vis->setFixedWidth(64);
    r.vis->setStyleSheet(QStringLiteral("margin-left:24px;"));
    hl->addWidget(r.vis);

    connect(r.btn, &QPushButton::clicked, this, [this, btn = r.btn] {
        const qint64 id = btn->property("rid").toLongLong();
        if (pickMode_) { emit routePicked(id); accept(); }
        else           { selectRow(id); }
    });
    connect(r.vis, &QCheckBox::toggled, this, [this, box = r.vis](bool on) {
        const qint64 id = box->property("rid").toLongLong();
        if (store_) store_->setRouteVisible(id, on);
    });
    return r;
}

void RouteListDialog::refresh() {
    if (!store_) return;
    const QVector<Route>& routes = store_->routes();

    countLabel_->setText(QStringLiteral("%1 route%2")
                         .arg(routes.size()).arg(routes.size() == 1 ? "" : "s"));

    const int want = routes.size();
    while (int(rows_.size()) < want) {
        Row r = makeRow();
        rowLayout_->insertWidget(rowLayout_->count() - 1, r.btn);
        rows_.push_back(r);
    }
    while (int(rows_.size()) > want) {
        rows_.back().btn->deleteLater();
        rows_.pop_back();
    }

    bool selectionStillExists = false;
    for (int i = 0; i < want; ++i) {
        const Route& rt = routes[i];
        Row& row = rows_[i];
        row.id = rt.id;
        row.btn->setProperty("rid", QVariant::fromValue(rt.id));
        row.vis->setProperty("rid", QVariant::fromValue(rt.id));
        const QString nm = rt.name.isEmpty() ? QStringLiteral("(unnamed)") : rt.name;
        if (row.name->text() != nm) row.name->setText(nm);
        row.meta->setText(QString::number(rt.points.size()));
        // Set checkbox without retriggering the store write.
        row.vis->blockSignals(true);
        row.vis->setChecked(rt.visible);
        row.vis->blockSignals(false);
        if (rt.id == selectedId_) selectionStillExists = true;
    }
    if (!selectionStillExists) selectedId_ = -1;
    restyleRows();
    const bool hasSel = selectedId_ >= 0;
    deleteBtn_->setEnabled(hasSel);
    propsBtn_->setEnabled(hasSel);
    if (editBtn_) editBtn_->setEnabled(hasSel);
}

void RouteListDialog::selectRow(qint64 id) {
    selectedId_ = (selectedId_ == id) ? -1 : id;   // tap again to deselect
    restyleRows();
    const bool hasSel = selectedId_ >= 0;
    deleteBtn_->setEnabled(hasSel);
    propsBtn_->setEnabled(hasSel);
    if (editBtn_) editBtn_->setEnabled(hasSel);
}

void RouteListDialog::restyleRows() {
    // The cell labels carry their own colour (they're transparent to the mouse),
    // so flip them to the title-foreground on the accent-highlighted selected row
    // to keep contrast; plain rows use the normal action foreground.
    const theme::MenuPalette& t = theme::menu();
    for (Row& r : rows_) {
        const bool sel = (r.id == selectedId_ && selectedId_ >= 0);
        r.btn->setChecked(sel);
        const QString cell = QStringLiteral(
            "font-size:14px; padding:0 4px; border:none; color:%1;")
            .arg(sel ? t.titleFg : t.actionFg);
        r.name->setStyleSheet(cell);
        r.meta->setStyleSheet(cell);
    }
}

void RouteListDialog::deleteSelected() {
    if (!store_ || selectedId_ < 0) return;
    const Route* r = store_->route(selectedId_);
    const QString nm = (r && !r->name.isEmpty()) ? r->name : QStringLiteral("this route");
    if (QMessageBox::question(this, QStringLiteral("Delete Route"),
            QStringLiteral("Delete %1?").arg(nm)) != QMessageBox::Yes)
        return;
    const qint64 id = selectedId_;
    selectedId_ = -1;
    store_->removeRoute(id);   // emits routesChanged -> refresh()
}
