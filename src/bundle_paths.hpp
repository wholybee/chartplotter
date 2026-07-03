#pragma once

// Resolves where the app's bundled read-only data and loadable plugins live at
// runtime.
//
// On Windows/Linux the app is a "portable directory": the data files sit next to
// the executable and the plugins in a plugins/ subfolder — the layout the CMake
// post-build steps assemble. On macOS the app is a .app bundle, whose executable
// lives in Contents/MacOS, and loose data files may NOT sit there: macOS code
// signing rejects non-code content in the MacOS dir, and nested plugin code
// belongs in PlugIns. So on macOS bundled data goes to Contents/Resources and
// plugins to Contents/PlugIns, and these helpers resolve to those.
//
// Keep this in sync with the deploy destinations in the top-level CMakeLists.txt
// (CHARTPLOTTER_DATA_DEPLOY_DIR / CHARTPLOTTER_PLUGIN_DEPLOY_DIR) and the plugin
// scan location in cmake/macdeploy.cmake.

#include <QCoreApplication>
#include <QDir>
#include <QString>

namespace bundlepaths {

// Directory holding bundled data files (gdal-data/, symbols.bin,
// rastersymbols-*.png, instruments.xml, ...).
inline QString dataDir() {
#ifdef Q_OS_MACOS
    return QDir::cleanPath(QCoreApplication::applicationDirPath()
                           + QStringLiteral("/../Resources"));
#else
    return QCoreApplication::applicationDirPath();
#endif
}

// Directory scanned for dynamically-loaded plugins (QPluginLoader).
inline QString pluginDir() {
#ifdef Q_OS_MACOS
    return QDir::cleanPath(QCoreApplication::applicationDirPath()
                           + QStringLiteral("/../PlugIns"));
#else
    return QCoreApplication::applicationDirPath() + QStringLiteral("/plugins");
#endif
}

} // namespace bundlepaths
