#pragma once
#include <QObject>
#include <QSet>
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
//
// Acknowledge (mute): the user can silence the alarm for one dangerous target
// from its quick-info popup. Muting only stops the sound — the target still shows
// as dangerous. The mute is cleared automatically once that target is no longer
// dangerous, so a fresh close encounter alarms again. The alarm still sounds for
// any *other* dangerous target that has not been acknowledged.
class AisAlarm : public QObject {
    Q_OBJECT
public:
    explicit AisAlarm(const AisTargetStore* store, QObject* parent = nullptr);

    // The rules deciding which targets are dangerous (kept in sync with the
    // overlay's copy by the host).
    void setRules(const DangerRules& rules);
    // Master on/off for the audible alarm. When turned off, any sounding alarm
    // stops immediately and outstanding acknowledgements are cleared.
    void setSoundEnabled(bool on);
    bool soundEnabled() const { return soundEnabled_; }

    // Is this target currently dangerous under the active rules?
    bool isDangerous(quint32 mmsi) const;
    // Has the user muted the alarm for this (dangerous) target?
    bool isAcknowledged(quint32 mmsi) const { return acked_.contains(mmsi); }
    // Mute the alarm for this target (no-op unless it is currently dangerous).
    void acknowledge(quint32 mmsi);

signals:
    // The set of acknowledged/dangerous targets changed — mute buttons refresh.
    void stateChanged();

private slots:
    void reevaluate();

private:
    bool shouldSound() const;   // any dangerous target that is not acknowledged
    void pruneAcknowledged();   // drop mutes for targets no longer dangerous
    void startSound();
    void stopSound();

    const AisTargetStore* store_ = nullptr;
    DangerRules   rules_;
    bool          soundEnabled_ = false;
    bool          sounding_ = false;      // is the effect currently looping?
    QSet<quint32> acked_;                 // muted dangerous targets, by MMSI
    QTimer*       timer_ = nullptr;
    QSoundEffect* effect_ = nullptr;      // null if the alarm WAV couldn't be built
};
