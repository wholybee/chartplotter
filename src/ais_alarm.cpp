#include "ais_alarm.hpp"
#include "ais_target_store.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QSoundEffect>
#include <QTimer>
#include <QUrl>
#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;

// ---- alarm tone synthesis --------------------------------------------------
// A short WAV that, looped, produces a harsh two-beep-then-pause alarm — the
// kind of piercing, pulsing tone meant to rouse a dozing watchkeeper. Built once
// at startup and written to a temp file for QSoundEffect to loop.
constexpr int    kSampleRate = 44100;
constexpr double kToneHz     = 2000.0;   // piercing square wave
constexpr double kAmplitude  = 0.75;     // fraction of full scale (loud, no clip)

void appendLE16(QByteArray& b, quint16 v) {
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
}
void appendLE32(QByteArray& b, quint32 v) {
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
    b.append(char((v >> 16) & 0xFF));
    b.append(char((v >> 24) & 0xFF));
}

// Append `seconds` of a square wave at `freq` (0 => silence) to a 16-bit PCM buffer.
void appendTone(QByteArray& pcm, double freq, double seconds) {
    const int n = int(kSampleRate * seconds);
    const qint16 hi = qint16(kAmplitude * 32767.0);
    for (int i = 0; i < n; ++i) {
        qint16 v = 0;
        if (freq > 0.0) {
            const double phase = std::fmod(freq * double(i) / kSampleRate, 1.0);
            v = phase < 0.5 ? hi : qint16(-hi);   // square wave
        }
        appendLE16(pcm, quint16(v));
    }
}

// Write the looping alarm WAV to a temp file; returns its path (empty on failure).
QString buildAlarmWav() {
    QByteArray pcm;
    appendTone(pcm, kToneHz, 0.14);   // beep
    appendTone(pcm, 0.0,     0.08);   // short gap
    appendTone(pcm, kToneHz, 0.14);   // beep
    appendTone(pcm, 0.0,     0.50);   // long gap -> "bi-bip .... bi-bip ...."

    const quint32 dataSize = quint32(pcm.size());
    QByteArray wav;
    wav.append("RIFF");
    appendLE32(wav, 36 + dataSize);
    wav.append("WAVE");
    wav.append("fmt ");
    appendLE32(wav, 16);                       // PCM fmt chunk size
    appendLE16(wav, 1);                        // audio format: PCM
    appendLE16(wav, 1);                        // channels: mono
    appendLE32(wav, kSampleRate);
    appendLE32(wav, kSampleRate * 2);          // byte rate (mono, 16-bit)
    appendLE16(wav, 2);                        // block align
    appendLE16(wav, 16);                       // bits per sample
    wav.append("data");
    appendLE32(wav, dataSize);
    wav.append(pcm);

    const QString path = QDir(QDir::tempPath()).filePath(
        QStringLiteral("hmvchartplotter_ais_alarm.wav"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    f.write(wav);
    f.close();
    return path;
}
}  // namespace

AisAlarm::AisAlarm(const AisTargetStore* store, QObject* parent)
    : QObject(parent), store_(store) {
    // Re-check ~1 Hz, matching the CPA calculator's cadence, so the alarm tracks
    // CPA/TCPA as they change and clears promptly once no threat remains.
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &AisAlarm::reevaluate);
    timer_->start();

    const QString wav = buildAlarmWav();
    if (!wav.isEmpty()) {
        effect_ = new QSoundEffect(this);
        effect_->setSource(QUrl::fromLocalFile(wav));
        effect_->setLoopCount(QSoundEffect::Infinite);
        effect_->setVolume(1.0);
    }
}

void AisAlarm::setRules(const DangerRules& rules) {
    rules_ = rules;
    reevaluate();
}

void AisAlarm::setSoundEnabled(bool on) {
    if (on == soundEnabled_) return;
    soundEnabled_ = on;
    reevaluate();
}

void AisAlarm::reevaluate() {
    if (soundEnabled_ && anyDangerous()) startSound();
    else                                 stopSound();
}

bool AisAlarm::anyDangerous() const {
    if (!store_) return false;
    for (const AisTarget& t : store_->targets())
        if (aisTargetDangerous(t, rules_)) return true;
    return false;
}

void AisAlarm::startSound() {
    if (sounding_ || !effect_) return;
    effect_->play();          // loops until stop() (LoopCount::Infinite)
    sounding_ = true;
}

void AisAlarm::stopSound() {
    if (!sounding_ || !effect_) return;
    effect_->stop();
    sounding_ = false;
}
