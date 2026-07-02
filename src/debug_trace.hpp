#pragma once
// src/debug_trace.hpp
//
// Opt-in shutdown/lifecycle tracing to a file. A GUI-subsystem app has no
// console, so qDebug() is not reliably visible; this appends timestamped
// milestones to a file we can read after a hang (e.g. a process that outlives
// its window). Entirely inert unless the environment variable HMV_SHUTDOWN_TRACE
// is set, in which case it names the log file (or "1"/"" -> a default in the
// temp dir). Kept dependency-light (Core only) so any TU can include it.

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>

namespace hmvtrace {

// Append one milestone line. Cheap no-op when tracing is disabled.
inline void mark(const char* what) {
    static const QString path = [] {
        const QByteArray v = qgetenv("HMV_SHUTDOWN_TRACE");
        if (v.isEmpty()) return QString();                 // disabled
        const QString s = QString::fromLocal8Bit(v);
        if (s == QLatin1String("1") || s == QLatin1String("true"))
            return QDir::temp().filePath(QStringLiteral("hmv_shutdown_trace.log"));
        return s;
    }();
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    const QString line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
                       + QLatin1String("  ") + QString::fromUtf8(what)
                       + QLatin1Char('\n');
    f.write(line.toUtf8());
    f.flush();   // survive a hard kill of the process right after
}

inline void mark(const QString& what) { mark(what.toUtf8().constData()); }

} // namespace hmvtrace
