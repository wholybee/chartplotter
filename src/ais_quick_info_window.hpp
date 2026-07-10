#pragma once
#include <QFrame>

class AisTargetStore;
class AisAlarm;
class QLabel;
class QPushButton;

// Small, frameless quick-look popup for one AIS target: name (or MMSI when no
// name is known), COG, and SOG. Opened by the first click on a target; a second
// click on the same target opens the full AisTargetInfoWindow. Meant as a
// glance that stays out of the way — the caller dismisses it on any chart
// interaction (empty click, pan, zoom). It refreshes live from the store and
// closes itself if the target ages out.
//
// A frameless Qt::Tool that shows without stealing focus, so the chart keeps
// receiving clicks/pans that drive the dismissal.
class AisQuickInfoWindow : public QFrame {
    Q_OBJECT
public:
    // `alarm` (optional) supplies the danger/mute state and receives the mute
    // action; when null, no mute button is shown.
    AisQuickInfoWindow(quint32 mmsi, const AisTargetStore* store,
                       AisAlarm* alarm = nullptr, QWidget* parent = nullptr);

    quint32 mmsi() const { return mmsi_; }

private:
    void refresh();

    quint32 mmsi_ = 0;
    const AisTargetStore* store_ = nullptr;
    AisAlarm* alarm_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* cogLabel_ = nullptr;
    QLabel* sogLabel_ = nullptr;
    QLabel* distLabel_ = nullptr;  // distance to ownship; shown when available
    QLabel* cpaLabel_ = nullptr;   // shown only when CPA/TCPA are available
    QLabel* tcpaLabel_ = nullptr;
    QPushButton* muteBtn_ = nullptr;  // shown only for a dangerous target when the alarm is on
};
