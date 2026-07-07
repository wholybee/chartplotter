#pragma once
#include "frameless_info_dialog.hpp"

class Settings;
class QPushButton;

// Small floating panel that toggles the chart display layers — Soundings,
// Symbols, Text, and Depth Contours. It reads and writes the same Settings
// flags the side menu exposes, so the button-panel and the menu stay in sync in
// both directions (Settings is the single source of truth; its change signals
// keep every view's checkmark current).
//
// Uses the shared dark "instrument panel" look of the other floating info
// dialogs (see FramelessInfoDialog / AisTargetInfoWindow).
class LayersDialog : public FramelessInfoDialog {
    Q_OBJECT
public:
    explicit LayersDialog(Settings* settings, QWidget* parent = nullptr);

private:
    // Build one full-width toggle row wired to a Settings flag, returning the
    // checkable button so the caller can connect its getter/setter/signal.
    QPushButton* makeToggle(const QString& label, bool on);

    Settings* settings_ = nullptr;
};
