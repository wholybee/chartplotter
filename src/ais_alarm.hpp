#pragma once
#include <QObject>
#include "ais_overlay.hpp"   // DangerRules

class AisTargetStore;
class QTimer;
class QSoundEffect;

// Sounds an audible alarm while any AIS target is dangerous.
//
// It shares the exact danger predicate the overlay uses (aisTargetDangerous), so
// the beep and the on-chart red flag always agree. Danger depends on CPA/TCPA,
// which evolve with time and vessel movement, so it re-evaluates on a 1 Hz tick
// (matching the CPA calculator) and starts/stops the sound as threats appear and
// clear. The alarm is a harsh, pulsing beep meant to wake a dozing watchkeeper;
// it only ever plays while the user has enabled the sound.
class AisAlarm : public QObject {
    Q_OBJECT
public:
    explicit AisAlarm(const AisTargetStore* store, QObject* parent = nullptr);

    // The rules deciding which targets are dangerous (kept in sync with the
    // overlay's copy by the host).
    void setRules(const DangerRules& rules);
    // Master on/off for the audible alarm. When turned off, any sounding alarm
    // stops immediately.
    void setSoundEnabled(bool on);

private slots:
    void reevaluate();

private:
    bool anyDangerous() const;
    void startSound();
    void stopSound();

    const AisTargetStore* store_ = nullptr;
    DangerRules   rules_;
    bool          soundEnabled_ = false;
    bool          sounding_ = false;      // is the effect currently looping?
    QTimer*       timer_ = nullptr;
    QSoundEffect* effect_ = nullptr;      // null if the alarm WAV couldn't be built
};
