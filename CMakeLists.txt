cmake_minimum_required(VERSION 3.16)
project(marine_chart_viewer_qt VERSION 0.5.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Network Sql WebSockets Multimedia)

# Prefer GDAL's own CMake config; fall back to pkg-config (e.g. MSYS2).
find_package(GDAL QUIET)
if(GDAL_FOUND)
    set(GDAL_LINK GDAL::GDAL)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GDAL REQUIRED IMPORTED_TARGET gdal)
    set(GDAL_LINK PkgConfig::GDAL)
endif()

# ---- Symbol atlas build-time tool ------------------------------------------
#
# gen_symbols parses data/chartsymbols.xml and emits symbols.bin: a flat binary
# lookup table mapping S-57 object-class names → symbol indices and symbol
# names → atlas-PNG rectangles + pivot offsets.
#
# The tool is a plain console executable (not a Qt GUI app).  It only needs
# Qt6::Core (QXmlStreamReader lives there, not in Qt6::Xml).
#
# NOTE: In a cross-compilation scenario the host (build-machine) tool must be
#       built separately.  For the common native-Windows build this is a
#       non-issue.

add_executable(gen_symbols tools/gen_symbols.cpp)
set_target_properties(gen_symbols PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(gen_symbols PRIVATE Qt6::Core)

# Run gen_symbols at build time to produce symbols.bin in the binary directory.
# The command re-runs whenever the tool exe or the XML source changes.
#
# gen_symbols links Qt6::Core dynamically, so Qt6Core.dll must be locatable when
# the tool runs during the build.  On Windows the Qt DLLs live in the Qt bin/
# directory, which is not normally on the build environment's PATH — without
# this the tool aborts with STATUS_DLL_NOT_FOUND (0xC0000135).  Prepend the Qt
# bin directory (the folder containing the Qt6::Core import library's runtime)
# to PATH for just this invocation.  `cmake -E env --modify` (CMake >= 3.25)
# handles the list separator correctly and leaves the rest of PATH intact.
add_custom_command(
    OUTPUT  "${CMAKE_CURRENT_BINARY_DIR}/symbols.bin"
    COMMAND ${CMAKE_COMMAND} -E env
            --modify "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
            "$<TARGET_FILE:gen_symbols>"
            "${CMAKE_CURRENT_SOURCE_DIR}/data/chartsymbols.xml"
            "${CMAKE_CURRENT_BINARY_DIR}/symbols.bin"
    DEPENDS gen_symbols
            "${CMAKE_CURRENT_SOURCE_DIR}/data/chartsymbols.xml"
    COMMENT "Generating symbol lookup table (symbols.bin) from chartsymbols.xml"
    VERBATIM
)
add_custom_target(symbols_bin ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/symbols.bin"
)

# Stage 3 unit test: legacy <-> normalized product-model round trip. GDAL-free,
# so it only needs the pure adapter and Qt6::Core. Registered with CTest; run
# with `ctest -C Release`.
enable_testing()
add_executable(test_product_model
    tools/test_product_model.cpp
    src/product_adapter.cpp
)
set_target_properties(test_product_model PROPERTIES WIN32_EXECUTABLE FALSE)
target_include_directories(test_product_model PRIVATE src)
target_link_libraries(test_product_model PRIVATE Qt6::Core)
add_test(NAME product_model_round_trip COMMAND test_product_model)
# Qt6Core.dll must be locatable when CTest runs the test exe (see gen_symbols).
set_tests_properties(product_model_round_trip PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Stage 5 unit test: ear-clip tessellator + prepared-render cache serialization
# round trip. The cache TU only needs Qt6::Core (no GUI / GDAL).
add_executable(test_prepared_render
    tools/test_prepared_render.cpp
    src/prepared_render_cache.cpp
)
set_target_properties(test_prepared_render PROPERTIES WIN32_EXECUTABLE FALSE)
target_include_directories(test_prepared_render PRIVATE src)
target_link_libraries(test_prepared_render PRIVATE Qt6::Core)
add_test(NAME prepared_render COMMAND test_prepared_render)
set_tests_properties(prepared_render PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Stage 7 (path A) step A3 unit test: the pure quilt / cell-visibility selection
# extracted from ChartView. Needs Qt6::Gui for QPainterPath; no GUI event loop or
# GDAL.
add_executable(test_chart_quilt
    tools/test_chart_quilt.cpp
    src/chart_quilt.cpp
)
set_target_properties(test_chart_quilt PROPERTIES WIN32_EXECUTABLE FALSE)
target_include_directories(test_chart_quilt PRIVATE src)
target_link_libraries(test_chart_quilt PRIVATE Qt6::Gui)
add_test(NAME chart_quilt COMMAND test_chart_quilt)
set_tests_properties(chart_quilt PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Stage 7 (path A) step A4 unit test: prepared-render -> GPU vertex batches.
# Needs Qt6::Gui (QColor / QPainterPath via cell_builder); no RHI, no GDAL.
add_executable(test_gpu_batches
    tools/test_gpu_batches.cpp
    src/gpu_batches.cpp
    src/cell_builder.cpp
)
set_target_properties(test_gpu_batches PROPERTIES WIN32_EXECUTABLE FALSE)
target_include_directories(test_gpu_batches PRIVATE src)
target_link_libraries(test_gpu_batches PRIVATE Qt6::Gui)
add_test(NAME gpu_batches COMMAND test_gpu_batches)
set_tests_properties(gpu_batches PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Stage 7 (path A) step A1: prove the QRhiWidget / qsb shader stack works on this
# machine, in isolation from the chart app. Manual harness (opens a window, needs a
# display + human eyes), so it is NOT a CTest. QRhiWidget lives in Qt6::Widgets
# (public API since Qt 6.7) and auto-selects the RHI backend per platform (Direct3D
# 11 on Windows, Vulkan/OpenGL on Linux, Metal on macOS); qt6_add_shaders needs the
# Qt6 ShaderTools (the qsb shader baker). A loud configure failure here means the
# GPU toolchain isn't installed.
find_package(Qt6 REQUIRED COMPONENTS ShaderTools)

# The QRhi* classes QRhiWidget exposes are private Gui API, so the backend needs
# Qt's private Gui headers. Upstream Qt and MSYS2 ship a Qt6::GuiPrivate CMake
# package; Debian/Ubuntu ship the private headers (qt6-base-private-dev) but
# deliberately omit the CMake package, so synthesise an equivalent target from the
# installed private include tree when Qt6::GuiPrivate is absent. Either way the
# implementation symbols come from the public Qt6::Gui library.
find_package(Qt6 QUIET COMPONENTS GuiPrivate)
if(TARGET Qt6::GuiPrivate)
    set(QT_GUI_PRIVATE Qt6::GuiPrivate)
else()
    set(_qt_gui_private_ok FALSE)
    get_target_property(_qt_gui_incs Qt6::Gui INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_inc IN LISTS _qt_gui_incs Qt6Gui_INCLUDE_DIRS)
        # Private headers live at <inc>/<ver>/QtGui, with the RHI headers under
        # .../QtGui/rhi/ (matching the code's #include <rhi/qrhi.h>).
        if(EXISTS "${_inc}/${Qt6Gui_VERSION}/QtGui/rhi/qrhi.h")
            add_library(Qt6GuiPrivateShim INTERFACE)
            target_include_directories(Qt6GuiPrivateShim INTERFACE
                "${_inc}/${Qt6Gui_VERSION}"
                "${_inc}/${Qt6Gui_VERSION}/QtGui")
            target_link_libraries(Qt6GuiPrivateShim INTERFACE Qt6::Gui)
            set(QT_GUI_PRIVATE Qt6GuiPrivateShim)
            set(_qt_gui_private_ok TRUE)
            break()
        endif()
    endforeach()
    if(NOT _qt_gui_private_ok)
        message(FATAL_ERROR
            "Qt6::GuiPrivate is unavailable and the private RHI headers were not "
            "found next to Qt6::Gui (looked for "
            "<inc>/${Qt6Gui_VERSION}/QtGui/rhi/qrhi.h). Install qt6-base-private-dev "
            "(Debian/Ubuntu) or use a Qt that ships the GuiPrivate CMake package.")
    endif()
endif()

# The standalone manual GPU harness bakes the same color.vert/color.frag shaders
# as chartviewer (below). Under the Xcode "new build system" a custom-command
# output (color.vert.qsb) may not be attached to two independent targets, so
# generating both in one Xcode project fails at the generate step. The harness is
# a manual, display-required tool (not a CTest) that nothing depends on, so
# default it OFF for the Xcode generator and ON everywhere else (Ninja / Make /
# Visual Studio dedup the shared shader output and build it fine). Override with
# -DCHARTPLOTTER_BUILD_GPU_HARNESS=ON/OFF.
if(CMAKE_GENERATOR STREQUAL "Xcode")
    set(_gpu_harness_default OFF)
else()
    set(_gpu_harness_default ON)
endif()
option(CHARTPLOTTER_BUILD_GPU_HARNESS
    "Build the standalone manual GPU canvas harness (test_gpu_canvas)"
    ${_gpu_harness_default})

if(CHARTPLOTTER_BUILD_GPU_HARNESS)
    qt_add_executable(test_gpu_canvas
        tools/test_gpu_canvas.cpp
        src/gpu_chart_view.cpp
        src/prepared_chart_cache.cpp
    )
    set_target_properties(test_gpu_canvas PROPERTIES WIN32_EXECUTABLE FALSE)
    target_include_directories(test_gpu_canvas PRIVATE src)
    target_link_libraries(test_gpu_canvas PRIVATE Qt6::Widgets ${QT_GUI_PRIVATE})
    qt6_add_shaders(test_gpu_canvas "gpu_test_shaders"
        BATCHABLE
        PREFIX "/shaders"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders"
        FILES src/shaders/color.vert src/shaders/color.frag
    )
endif()

qt_add_executable(chartviewer
    src/main.cpp
    src/main_window.cpp
    src/side_menu.cpp
    src/settings.cpp
    src/chart_sets_dialog.cpp
    src/prepare_cache_dialog.cpp
    src/units_dialog.cpp
    src/navigation_options_dialog.cpp
    src/touch_spin_box.cpp
    src/stale_thresholds_dialog.cpp
    src/ownship_prediction_dialog.cpp
    src/nmea0183_debug_window.cpp
    src/nmea0183_plugin.cpp
    src/nmea0183_nav_sender.cpp
    src/nmea2000_client.cpp
    src/nmea2000_plugin.cpp
    src/n2k_decoder.cpp
    src/n2k_nav_sender.cpp
    src/actisense_ascii_parser.cpp
    src/nav_data_browser_window.cpp
    src/data_priority_dialog.cpp
    src/chart_detail_dialog.cpp
    src/chart_symbol_size_dialog.cpp
    src/ship_size_dialog.cpp
    src/ownship_mmsi_dialog.cpp
    src/heading_source_dialog.cpp
    src/dangerous_ships_dialog.cpp
    src/tracking_dialog.cpp
    src/core_api.cpp
    src/plugin_manager.cpp
    src/chart_view.cpp
    src/chart_catalog.cpp
    src/chart_loader.cpp
    src/prepared_chart_cache.cpp
    src/product_adapter.cpp
    src/product_decoder.cpp
    src/mbtiles_reader.cpp
    src/mbtiles_service.cpp
    src/nav_data_store.cpp
    src/ais_target_store.cpp
    src/cpa_calculator.cpp
    src/ais_decoder.cpp
    src/ais_overlay.cpp
    src/ais_alarm.cpp
    src/about_dialog.cpp
    src/frameless_info_dialog.cpp
    src/layers_dialog.cpp
    src/ais_target_info_window.cpp
    src/ais_target_list_dialog.cpp
    src/ais_quick_info_window.cpp
    src/route_store.cpp
    src/route_navigator.cpp
    src/track_recorder.cpp
    src/nav_display_window.cpp
    src/route_overlay.cpp
    src/route_waypoint_dialog.cpp
    src/route_properties_dialog.cpp
    src/waypoint_properties_dialog.cpp
    src/track_properties_dialog.cpp
    src/route_quick_info_window.cpp
    src/chart_object_info_window.cpp
    src/name_dialog.cpp
    src/vessel_symbol.cpp
    src/sym_atlas.cpp
    src/render_resource_atlas.cpp
    src/portrayal_engine.cpp
    src/render_scene_compiler.cpp
    src/prepared_render_cache.cpp
    src/cell_builder.cpp
    src/cell_source.cpp
    src/chart_quilt.cpp
    src/gpu_batches.cpp
    src/gpu_chart_view.cpp
    src/theme.cpp
    src/nmea0183_client.cpp
)

if(MSVC)
    target_compile_options(chartviewer PRIVATE
        $<$<CONFIG:Release>:/Zi>
    )

    target_link_options(chartviewer PRIVATE
        $<$<CONFIG:Release>:/DEBUG:FULL>
    )

    set_target_properties(chartviewer PROPERTIES
        COMPILE_PDB_NAME "hmvchartplotter"
        COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
        PDB_NAME "hmvchartplotter"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    )
endif()

target_link_libraries(chartviewer PRIVATE Qt6::Widgets Qt6::Concurrent Qt6::Network Qt6::Sql Qt6::Multimedia ${QT_GUI_PRIVATE} ${GDAL_LINK})
add_dependencies(chartviewer symbols_bin)

# Stage 7: bake the retained-GPU shaders into the app (same qsb artifacts the
# harness uses), so gpu_chart_view can load :/shaders/color.*.qsb at runtime.
qt6_add_shaders(chartviewer "chart_shaders"
    BATCHABLE
    PREFIX "/shaders"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders"
    FILES src/shaders/color.vert src/shaders/color.frag
          src/shaders/tex.vert   src/shaders/tex.frag
)

# Embed the README and licence so the in-app About box can show them without an
# on-disk lookup (BASE makes them resolve as :/README.md and :/LICENSE).
qt_add_resources(chartviewer "appdocs"
    PREFIX "/"
    BASE "${CMAKE_SOURCE_DIR}"
    FILES
        README.md
        LICENSE)

# Dev convenience: lets the basemap resolver fall back to the in-tree gshhg-shp
# folder during development. On an installed build this path won't exist, so it
# is skipped and the standard data locations are used instead.
target_compile_definitions(chartviewer PRIVATE
    CHARTPLOTTER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    # Application version string for the About box / window metadata.
    APP_VERSION="${PROJECT_VERSION}"
    # Flips HOST_EXPORT to dllexport in the host's translation units; plugins
    # don't define this, so they see dllimport for the same types.
    CHARTPLOTTER_BUILD_HOST=1)

set_target_properties(chartviewer PROPERTIES
    # User-facing executable name (hmvchartplotter.exe).
    OUTPUT_NAME "hmvchartplotter"
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    # Stable bundle id for the .app (code signing + a unique identifier). Without
    # it Qt warns it can't derive one from Xcode prefs and falls back to a
    # non-unique default. Matches the org name set in main.cpp (net.holybee).
    MACOSX_BUNDLE_GUI_IDENTIFIER "net.holybee.hmvchartplotter"
    # Expose the host's symbols so dynamically loaded plugins can link against
    # them. On Windows ENABLE_EXPORTS produces an import library (chartviewer.lib)
    # and WINDOWS_EXPORT_ALL_SYMBOLS auto-generates a .def listing every global
    # symbol from the exe's object files (including MOC-emitted metaobjects and
    # signal stubs). On Linux/macOS ENABLE_EXPORTS adds -rdynamic so the
    # dynamic linker honours dlopen lookups against the exe.
    ENABLE_EXPORTS TRUE
    WINDOWS_EXPORT_ALL_SYMBOLS TRUE
)

# ---- Where post-build steps stage bundled data and plugins ------------------
#
# On Windows/Linux the app is a portable directory: data files sit next to the
# exe and plugins in a plugins/ subfolder. On a macOS .app the executable lives
# in Contents/MacOS, where loose data files may NOT go (code signing rejects
# non-code content in the MacOS dir) and nested plugin code belongs in PlugIns —
# so data is staged into Contents/Resources and plugins into Contents/PlugIns.
# These variables are consumed by the data-copy steps below AND by each plugin's
# post-build copy (they inherit into the plugins/ subdirectories). Runtime
# resolution of these same locations lives in src/bundle_paths.hpp — keep the two
# in sync.
if(APPLE)
    set(CHARTPLOTTER_DATA_DEPLOY_DIR   "$<TARGET_BUNDLE_CONTENT_DIR:chartviewer>/Resources")
    set(CHARTPLOTTER_PLUGIN_DEPLOY_DIR "$<TARGET_BUNDLE_CONTENT_DIR:chartviewer>/PlugIns")
else()
    set(CHARTPLOTTER_DATA_DEPLOY_DIR   "$<TARGET_FILE_DIR:chartviewer>")
    set(CHARTPLOTTER_PLUGIN_DEPLOY_DIR "$<TARGET_FILE_DIR:chartviewer>/plugins")
endif()

# ---- Qt DLL deployment (Windows only) --------------------------------------
#
# After each build, run windeployqt to copy the Qt DLLs, platform plugins,
# and QML/styles needed to run the exe in-place.  windeployqt lives next to
# the Qt6::Core DLL, so we derive its path from the Core import location
# rather than hard-coding the Qt install prefix.
#
# --no-compiler-runtime  skip the MSVC redistributable (already present on dev
#                        machines; add an installer step for distribution).
# --no-translations      skip translation catalogues (saves ~30 MB in the
#                        output folder; re-enable if you add i18n support).
if(WIN32)
    get_target_property(_qt_core_loc Qt6::Core IMPORTED_LOCATION_RELEASE)
    if(NOT _qt_core_loc)
        get_target_property(_qt_core_loc Qt6::Core IMPORTED_LOCATION)
    endif()
    get_filename_component(_qt_bin_dir "${_qt_core_loc}" DIRECTORY)
    find_program(WINDEPLOYQT windeployqt HINTS "${_qt_bin_dir}" NO_DEFAULT_PATH)

    if(WINDEPLOYQT)
        message(STATUS "windeployqt found: ${WINDEPLOYQT}")
        add_custom_command(TARGET chartviewer POST_BUILD
            COMMAND "${WINDEPLOYQT}"
                $<IF:$<CONFIG:Debug>,--debug,--release>
                --no-compiler-runtime
                --no-translations
                "$<TARGET_FILE:chartviewer>"
            COMMENT "Running windeployqt to bundle Qt DLLs"
            VERBATIM
        )
    else()
        message(WARNING "windeployqt not found — Qt DLLs will not be copied automatically.")
    endif()
endif()

# ---- Bundle GDAL data files -------------------------------------------------
# S-57 charts need s57objectclasses.csv (and a few companion CSVs) to map the
# raw OBJL integer codes in each ENC file to named object classes like DEPARE,
# LNDARE, COALNE.  Without them GDAL reads geometry fine but returns empty/raw
# layer names — the chart loader's classify() function can't match anything, so
# every feature renders without fill or colour (grey outlines only).
#
# Strategy: find the data dir relative to the GDAL library that CMake resolved,
# then copy it into gdal-data/ next to the exe at build time. main.cpp reads
# applicationDirPath()/gdal-data and passes it to chart::init(), which calls
# CPLSetConfigOption("GDAL_DATA", ...) before GDALAllRegister().

find_path(GDAL_DATA_DIR
    NAMES s57objectclasses.csv
    HINTS "$ENV{GDAL_DATA}"
    NO_DEFAULT_PATH
)

if(NOT GDAL_DATA_DIR AND TARGET GDAL::GDAL)
    # Derive from the GDAL library location: lib → prefix → share/gdal.
    foreach(_prop
            IMPORTED_LOCATION_RELEASE
            IMPORTED_LOCATION_RELWITHDEBINFO
            IMPORTED_LOCATION_MINSIZEREL
            IMPORTED_LOCATION_DEBUG
            IMPORTED_LOCATION)
        if(NOT _gdal_lib_path)
            get_target_property(_gdal_lib_path GDAL::GDAL ${_prop})
        endif()
    endforeach()
    if(_gdal_lib_path)
        get_filename_component(_gdal_lib_dir "${_gdal_lib_path}" DIRECTORY)
        find_path(GDAL_DATA_DIR s57objectclasses.csv
            HINTS
                "${_gdal_lib_dir}/../share/gdal"
                "${_gdal_lib_dir}/../../share/gdal"
                "${_gdal_lib_dir}/gdal"
                "${_gdal_lib_dir}/gdal-data"
            NO_DEFAULT_PATH
        )
    endif()
endif()

if(GDAL_DATA_DIR)
    message(STATUS "GDAL data directory found: ${GDAL_DATA_DIR}")
    add_custom_command(TARGET chartviewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${GDAL_DATA_DIR}"
            "${CHARTPLOTTER_DATA_DEPLOY_DIR}/gdal-data"
        COMMENT "Bundling GDAL data files into gdal-data/"
        VERBATIM
    )
else()
    message(WARNING
        "GDAL data directory not found.\n"
        "S-57 charts will render without colours on machines without GDAL installed.\n"
        "Set GDAL_DATA_DIR=<path containing s57objectclasses.csv> and re-run CMake.")
endif()

# ---- Symbol atlas data files ------------------------------------------------
#
# Copy the prebaked symbols.bin and the atlas PNG(s) into the bundled data dir
# (next to the exe, or Contents/Resources on macOS) so ChartView can find them at
# runtime via bundlepaths::dataDir().
#
# Place the source files in data/ under the project root:
#   data/chartsymbols.xml       (input to gen_symbols — build time only)
#   data/rastersymbols-day.png  (OpenCPN day-mode sprite sheet)
#   data/rastersymbols-dusk.png (optional — dusk variant)
#   data/rastersymbols-dark.png (optional — night variant)

# symbols.bin is generated in the binary directory by the custom command above.
# make_directory first: the data dir already exists next to the exe, but on macOS
# Contents/Resources may not exist yet (copy_if_different won't create it).
add_custom_command(TARGET chartviewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CHARTPLOTTER_DATA_DEPLOY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/symbols.bin"
        "${CHARTPLOTTER_DATA_DEPLOY_DIR}/symbols.bin"
    COMMENT "Installing symbols.bin into the bundled data dir"
    VERBATIM
)

# Atlas PNGs: copy each one that is present in data/.  Missing variants are
# silently skipped so a partial data/ directory doesn't break the build.
foreach(_atlas_png
        rastersymbols-day.png
        rastersymbols-dusk.png
        rastersymbols-dark.png)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/data/${_atlas_png}")
    if(EXISTS "${_src}")
        add_custom_command(TARGET chartviewer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CHARTPLOTTER_DATA_DEPLOY_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_src}"
                "${CHARTPLOTTER_DATA_DEPLOY_DIR}/${_atlas_png}"
            COMMENT "Installing ${_atlas_png} into the bundled data dir"
            VERBATIM
        )
    else()
        message(STATUS
            "Symbol atlas PNG not found (optional): ${_src}\n"
            "  Copy rastersymbols-*.png files into data/ to enable atlas symbols.")
    endif()
endforeach()

# ---- Dynamic plugins --------------------------------------------------------
#
# Each plugin lives in its own subdirectory under plugins/, builds to its own
# shared library (a separate target / VS project), and post-build copies itself
# into plugins/ next to the host exe so QPluginLoader picks it up at runtime.
add_subdirectory(plugins/signalk_plugin)
add_subdirectory(plugins/gpx_plugin)
add_subdirectory(plugins/wmm_plugin)
add_subdirectory(plugins/instruments_plugin)
# NOTE: the CM93 reader is GPL-2.0 (derived from OpenCPN) and lives in its own
# separate repository (chartplotter-cm93), built against the plugin SDK headers
# in src/ (chart_source.hpp et al.). It is not built here; drop its built
# chartplotter_cm93_plugin.dll into plugins/ next to the exe (with Qt6Concurrent.dll
# next to the exe) to enable CM93 charts. See docs/cm93.md.

# ---- Qt framework deployment: self-contained .app (macOS only) --------------
#
# macOS analogue of the Windows windeployqt step. macdeployqt copies the Qt
# frameworks and the Qt platform/style plugins into the built .app and rewrites
# the host exe's framework references to @rpath, so it launches on a Mac without
# Qt (or Homebrew) installed. cmake/macdeploy.cmake also fixes the app's own
# plugins (Contents/MacOS/plugins) and ad-hoc re-signs the bundle.
#
# Unlike windeployqt (a chartviewer POST_BUILD), this is a standalone target in
# ALL that depends on the exe AND every plugin: the plugins are copied into the
# bundle by their own post-build steps, which run *after* chartviewer, so a
# chartviewer POST_BUILD would deploy before the plugins exist. Turn off with
# -DCHARTPLOTTER_MACOS_DEPLOY=OFF (e.g. for a fast inner-loop build that just runs
# against the Qt libraries at their build-time paths).
if(APPLE)
    option(CHARTPLOTTER_MACOS_DEPLOY
        "Run macdeployqt after build to make the .app self-contained" ON)

    # Locate macdeployqt: prefer the imported tool target Qt ships; otherwise
    # find it next to qmake (same bin/ dir), then fall back to PATH.
    if(TARGET Qt6::macdeployqt)
        set(_macdeployqt "$<TARGET_FILE:Qt6::macdeployqt>")
    else()
        set(_macdeployqt_hint "")
        if(TARGET Qt6::qmake)
            get_target_property(_qmake_loc Qt6::qmake IMPORTED_LOCATION)
            get_filename_component(_macdeployqt_hint "${_qmake_loc}" DIRECTORY)
        endif()
        find_program(MACDEPLOYQT macdeployqt HINTS "${_macdeployqt_hint}")
        set(_macdeployqt "${MACDEPLOYQT}")
    endif()

    if(CHARTPLOTTER_MACOS_DEPLOY AND _macdeployqt)
        add_custom_target(macdeploy ALL
            COMMAND ${CMAKE_COMMAND}
                    -DAPP=$<TARGET_BUNDLE_DIR:chartviewer>
                    -DMACDEPLOYQT=${_macdeployqt}
                    -P ${CMAKE_SOURCE_DIR}/cmake/macdeploy.cmake
            COMMENT "Bundling Qt frameworks into the .app (macdeployqt)"
            VERBATIM
        )
        # Order after the exe + all plugins so their deployed output is in the
        # bundle before macdeployqt fixes it (same rationale as the Windows
        # installer target below).
        add_dependencies(macdeploy chartviewer
            signalk_plugin gpx_plugin wmm_plugin instruments_plugin)
    elseif(CHARTPLOTTER_MACOS_DEPLOY AND NOT _macdeployqt)
        message(WARNING
            "macdeployqt not found — the .app will NOT be self-contained (it will "
            "rely on the Qt frameworks at their build-time paths). Ensure Qt's "
            "deploy tools are installed, or set -DCHARTPLOTTER_MACOS_DEPLOY=OFF.")
    endif()
endif()

# ---- Packaging: Windows installer (CPack + NSIS) ----------------------------
#
# Wraps the fully-deployed Release application directory (chartviewer.exe plus
# the Qt/vcpkg DLLs, platform plugins, plugins/, gdal-data/, and data files that
# the post-build windeployqt / vcpkg / plugin steps assemble next to the exe)
# into a single self-contained NSIS installer .exe.
#
# Usage — build Release first, then run cpack from the build dir:
#
#   cmake --build build/vs2022 --config Release
#   cpack --config build/vs2022/CPackConfig.cmake -C Release -G NSIS
#
# (or: cd build/vs2022 && cpack -C Release -G NSIS)
#
# Requires NSIS (makensis) on PATH for the NSIS generator:
#   https://nsis.sourceforge.io/Download   (zlib/libpng licence)
# `cmake --install build/vs2022 --config Release --prefix <dir>` stages the same
# tree without NSIS, which is handy for verifying the payload.
if(WIN32)
    # The exe is deployed with windeployqt --no-compiler-runtime, so the MSVC
    # runtime is not copied next to it. Bundle it app-locally (msvcp140.dll,
    # vcruntime140*.dll, ...) so the app launches on a clean machine. The
    # Universal CRT ships with Windows 10/11, so it is intentionally not bundled.
    #
    # SKIP=TRUE lets the module resolve the DLL list into
    # CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS without adding its own install rule (which
    # defaults to bin/); we then install them next to the exe ourselves.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
            DESTINATION "."
            CONFIGURATIONS Release)

    # Install the complete, already-assembled Release directory. install(DIRECTORY)
    # cannot take a generator expression, so the per-config output path is named
    # directly; the CONFIGURATIONS guard keeps this to Release. Requires a prior
    # Release build (the directory is populated by the post-build steps).
    install(DIRECTORY "${CMAKE_BINARY_DIR}/Release/"
            DESTINATION "."
            CONFIGURATIONS Release
            USE_SOURCE_PERMISSIONS
            PATTERN "*.lib"      EXCLUDE   # dev-only artifacts
            PATTERN "*.exp"      EXCLUDE
            PATTERN "*.pdb"      EXCLUDE
            PATTERN "*.ilk"      EXCLUDE
            PATTERN "CMakeFiles" EXCLUDE)

    set(CPACK_GENERATOR "NSIS")
    # Emit the installer (and CPack's _CPack_Packages scratch dir) inside the
    # build tree rather than the caller's CWD, so running cpack from the repo
    # root doesn't drop artifacts into the source tree.
    set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/installer")
    set(CPACK_PACKAGE_NAME            "HMV Chartplotter")
    set(CPACK_PACKAGE_VENDOR          "net.holybee")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "HMV marine chart viewer / chartplotter")
    # Bump for each release; drives the installer filename and Add/Remove entry.
    set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
    set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
    set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "HMV Chartplotter")   # Program Files subfolder
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/README.md")

    # Start-menu / desktop shortcuts. NOTE: CPACK_PACKAGE_EXECUTABLES is not used
    # because the NSIS generator hardcodes a "bin\" prefix in the shortcut target
    # (it assumes binaries live in bin/), which is wrong for our flat layout where
    # hmvchartplotter.exe sits at the install root. Create the shortcuts directly so
    # they point at $INSTDIR\hmvchartplotter.exe, and remove them on uninstall.
    # Backslashes are quadrupled on purpose: CMake collapses "\\\\" to "\\", CPack
    # writes that verbatim into CPackConfig.cmake, and re-reading it yields a
    # single "\" that lands in the generated .nsi as the NSIS path separator.
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\HMV Chartplotter.lnk' '$INSTDIR\\\\hmvchartplotter.exe'
         CreateShortCut '$DESKTOP\\\\HMV Chartplotter.lnk' '$INSTDIR\\\\hmvchartplotter.exe'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\HMV Chartplotter.lnk'
         Delete '$DESKTOP\\\\HMV Chartplotter.lnk'")

    # NSIS specifics: 64-bit Program Files, clean reinstall, no PATH edits.
    set(CPACK_NSIS_DISPLAY_NAME "HMV Chartplotter")
    set(CPACK_NSIS_PACKAGE_NAME "HMV Chartplotter")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)

    include(CPack)

    # ---- Auto-build the NSIS installer as part of a Release build ------------
    #
    # Folds `cpack --config build/.../CPackConfig.cmake -C Release -G NSIS` into
    # the normal build so the installer is produced without a separate command.
    # The `installer` target is in ALL (so it runs on every build) but only
    # actually packages on a Release build (pack_release.cmake guards on $<CONFIG>,
    # since the install payload is Release-only). It depends on the exe and every
    # plugin so it runs *after* all of them — including their post-build deploy
    # steps (windeployqt, plugin DLLs, gdal-data) — have populated the Release dir
    # that CPack packages.
    #
    # Gated to MULTI-CONFIG generators (Visual Studio): the install(DIRECTORY
    # .../Release/ ...) rule above assumes the build/<Config>/ layout that only a
    # multi-config generator produces, and the payload bundles the MSVC runtime.
    # A single-config build — notably the Ninja/MinGW CI job — sets no
    # CMAKE_CONFIGURATION_TYPES, so this target is NOT created there. That keeps
    # CI from invoking cpack/NSIS (no makensis on those runners) and from
    # packaging the wrong layout. Requires NSIS (makensis); turn off with
    # -DCHARTPLOTTER_BUILD_INSTALLER=OFF.
    option(CHARTPLOTTER_BUILD_INSTALLER
        "Auto-build the NSIS installer during a multi-config Release build" ON)
    if(CHARTPLOTTER_BUILD_INSTALLER AND CMAKE_CONFIGURATION_TYPES)
        add_custom_target(installer ALL
            COMMAND ${CMAKE_COMMAND}
                    -DCFG=$<CONFIG>
                    -DCPACK_CMD=${CMAKE_CPACK_COMMAND}
                    -DCPACK_CONFIG=${CMAKE_BINARY_DIR}/CPackConfig.cmake
                    -DBIN_DIR=${CMAKE_BINARY_DIR}
                    -P ${CMAKE_SOURCE_DIR}/cmake/pack_release.cmake
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Packaging NSIS installer (Release only)"
            VERBATIM
        )
        # Order after the exe + all plugins so their deployed output is in place.
        add_dependencies(installer chartviewer
            signalk_plugin gpx_plugin wmm_plugin instruments_plugin)
    endif()
endif()

