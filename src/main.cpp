#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QSettings>
#include "chart_loader.hpp"
#include "main_window.hpp"
#include "app_info.hpp"
#include "app_log.hpp"
#include "bundle_paths.hpp"
#include "debug_trace.hpp"

int main(int argc, char** argv) {
    // Share GL resources across all contexts. The GPU chart layer is a
    // QRhiWidget composited into the top-level window's RHI backing store; on the
    // OpenGL path (Linux/Raspberry Pi) that composition is more reliable when
    // contexts share. Must be set before QApplication is constructed.
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // QApplication must come first: we need applicationDirPath() to locate the
    // bundled gdal-data/ folder before initialising GDAL.
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("net.holybee"));
    // The QSettings key stays "HMV Chart" so existing user settings aren't
    // orphaned by the rename; the user-visible name is the display name.
    QApplication::setApplicationName(QStringLiteral("HMV Chart"));
    QApplication::setApplicationDisplayName(appinfo::name());
    QApplication::setApplicationVersion(appinfo::version());
    // Window/taskbar icon on every platform. On Windows the exe also carries the
    // icon via resources/appicon.rc (used by Explorer and the installer); this
    // ensures the icon shows at runtime on Linux/macOS too. Embedded as a Qt
    // resource by CMake (:/appicon.png).
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/appicon.png")));

    // Install the file-logging message handler as early as possible so startup
    // messages are captured, then apply the persisted opt-in state. The handler
    // always chains to the default (console) handler; it only writes a file while
    // enabled. The live Settings object drives later on/off changes from the menu.
    // (Key must match settings.cpp's kLogToFile.)
    applog::install();
    applog::setEnabled(QSettings().value(QStringLiteral("system/logToFile"), false).toBool());
    // One self-describing startup line: which build, Qt version, and QPA platform
    // (e.g. "wayland" vs "xcb") — the context needed to read a GPU/RHI report.
    qInfo().noquote() << "Startup:" << appinfo::name() << appinfo::version()
                      << "| Qt" << qVersion()
                      << "| platform" << QGuiApplication::platformName();

    // Resolve the bundled GDAL data directory (contains s57objectclasses.csv
    // etc.). CMake copies this from the GDAL installation next to the executable
    // (Contents/Resources on macOS) at build time, so it travels with the app on
    // any machine regardless of whether GDAL is installed system-wide.
    std::string gdalDataDir;
    {
        const QDir d(bundlepaths::dataDir() + QStringLiteral("/gdal-data"));
        if (d.exists())
            gdalDataDir = d.absolutePath().toStdString();
    }

    // Registers GDAL drivers and S-57 options; sets GDAL_DATA if we found the
    // bundle. Must be called before any worker threads are spawned.
    chart::init(gdalDataDir);

#if defined(Q_OS_LINUX)
    // Belt-and-braces for the GPU chart layer (a QRhiWidget composited into the
    // top-level window's backing store). Qt decides ONCE, when a window's native
    // handle is created, whether that window composites through the RHI — by
    // scanning the widget tree for render-to-texture widgets. Any code path that
    // creates the window before the GPU layer exists locks the window to raster
    // and the GPU backend can never initialize ("QRhiWidget: No QRhi"). The known
    // such path (restoreGeometry with a saved maximized state) is fixed by
    // ordering in MainWindow's constructor; this forcing removes the whole class
    // of problem on Linux by making every window RHI-composited up front, which
    // also matches what was field-verified on Raspberry Pi 4 (V3D/Wayland).
    // Gate on the GPU preference so CPU-mode users on GL-less systems are never
    // forced onto RHI, and honour an explicit override from the environment.
    // Backend pinned to OpenGL to match GpuChartView's api. (Key matches
    // settings.cpp kRenderBackend.)
    if (QSettings().value(QStringLiteral("display/renderBackend")).toString()
            == QLatin1String("gpu")) {
        if (!qEnvironmentVariableIsSet("QT_WIDGETS_RHI"))
            qputenv("QT_WIDGETS_RHI", "1");
        if (!qEnvironmentVariableIsSet("QT_WIDGETS_RHI_BACKEND"))
            qputenv("QT_WIDGETS_RHI_BACKEND", "opengl");
    }
#endif

    MainWindow w;
    w.show();
    const int rc = app.exec();
    hmvtrace::mark("app.exec() returned; destroying MainWindow");
    // MainWindow `w` is destroyed as this scope unwinds (after the trace above);
    // a final "clean exit" line is written by its destructor chain. If the trace
    // file ends before that, teardown blocked — the last line names where.
    return rc;
}
