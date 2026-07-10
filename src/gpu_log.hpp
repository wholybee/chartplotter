#pragma once
// src/gpu_log.hpp
//
// Always-on, file-based GPU/RHI lifecycle log.
//
// The GPU black-screen fault is random and cannot be reproduced on demand, and a
// GUI-subsystem app has no console, so qDebug()/qCDebug() are not reliably
// visible in the field. To ever diagnose it we must have a persistent record
// written whenever it happens — without the user needing to arm anything in
// advance. This appends a handful of timestamped lines per launch (device
// creation, first frame, device loss, watchdog fallback) to hmv_gpu.log in the
// temp directory. It truncates once per process if the file has grown past a
// small cap, so it can never grow without bound. Dependency-light (Core only).

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace gpulog {

// Absolute path of the log file (temp dir / hmv_gpu.log). Shown to the user when
// the GPU path falls back, so they know where to find it.
inline QString path() {
    return QDir::temp().filePath(QStringLiteral("hmv_gpu.log"));
}

// Append one timestamped line. Best-effort: silently does nothing if the file
// can't be opened. Flushed immediately so a line survives a hard kill (or a
// driver-induced process death) right after it is written.
inline void write(const QString& msg) {
    const QString p = path();
    // Bound growth: the first write of each process truncates the file if it is
    // already large. GPU lifecycle events are a few lines per launch, so the cap
    // holds many sessions of history before rolling.
    static bool capChecked = false;
    QIODevice::OpenMode mode = QIODevice::Append | QIODevice::Text;
    if (!capChecked) {
        capChecked = true;
        if (QFileInfo(p).size() > 256 * 1024)
            mode = QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text;
    }
    QFile f(p);
    if (!f.open(mode)) return;
    f.write((QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
             + QLatin1String("  ") + msg + QLatin1Char('\n')).toUtf8());
    f.flush();
}

} // namespace gpulog
