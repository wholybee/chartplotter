# macOS bundle deployment — invoked from CMakeLists.txt via:
#
#   cmake -DAPP=<hmvchartplotter.app> -DMACDEPLOYQT=<macdeployqt> \
#         -DBUNDLE_SCRIPT=<cmake/macos_bundle_libs.sh> \
#         [-DHOMEBREW_LIB=<prefix/lib>] [-DGDAL_LIB_DIR=<dir>] \
#         -P cmake/macdeploy.cmake
#
# Makes the built .app self-contained (the macOS analogue of windeployqt): the Qt
# frameworks/plugins AND the non-Qt library closure (GDAL and its deps: GEOS,
# PROJ, SQLite, TIFF, OpenEXR, aws-sdk, ...) are copied into the bundle and their
# references rewritten so it runs on a Mac without Homebrew/Qt installed.
#
# The work is split:
#   1. macdeployqt          — bundles the Qt frameworks + Qt platform/style
#      plugins + qt.conf, and fixes our app plugins' Qt refs. It also makes a
#      shallow, incomplete pass at libgdal's non-Qt deps (misses e.g. libgeos,
#      OpenEXR's libIex), so the app would abort at launch on its own.
#   2. macos_bundle_libs.sh — recurses the *non-Qt* closure and rewrites every
#      reference to an explicit @executable_path/../Frameworks path.
# We use our own script rather than dylibbundler: dylibbundler's rpath rewiring is
# fragile and not idempotent — on a clean build it aborts trying to migrate an
# rpath (@executable_path/../lib) that the Homebrew libraries never had.

if(NOT EXISTS "${APP}")
    message(FATAL_ERROR "macdeploy: app bundle not found: ${APP}")
endif()
if(NOT MACDEPLOYQT)
    message(FATAL_ERROR "macdeploy: MACDEPLOYQT not set")
endif()
if(NOT BUNDLE_SCRIPT OR NOT EXISTS "${BUNDLE_SCRIPT}")
    message(FATAL_ERROR "macdeploy: BUNDLE_SCRIPT not found: ${BUNDLE_SCRIPT}")
endif()

# ---- 1. Qt frameworks/plugins via macdeployqt -------------------------------
# The app's own plugins are copied into Contents/PlugIns by each plugin target's
# post-build step (the host loads bundlepaths::pluginDir() == Contents/PlugIns).
# Pass each with -executable= so their Qt references are rewritten to @rpath too.
file(GLOB _plugins
    "${APP}/Contents/PlugIns/chartplotter_*.so"
    "${APP}/Contents/PlugIns/chartplotter_*.dylib")

set(_plugin_args "")
foreach(_p IN LISTS _plugins)
    list(APPEND _plugin_args "-executable=${_p}")
endforeach()

message(STATUS "macdeployqt: ${APP}")
foreach(_p IN LISTS _plugins)
    message(STATUS "  + fixing plugin: ${_p}")
endforeach()

execute_process(
    COMMAND "${MACDEPLOYQT}" "${APP}" ${_plugin_args} -verbose=1
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "macdeployqt failed (exit ${_rc})")
endif()

# ---- 2. Give the exe + plugins an rpath into Frameworks ---------------------
# So the Qt references macdeployqt rewrote to @rpath/QtCore.framework/... resolve
# to the bundled copies. macdeployqt reliably adds this to the exe only, and even
# then it was observed to keep only the build-time Homebrew Qt rpath — so add it
# explicitly to the exe(s) and every plugin. @executable_path always resolves to
# the host binary's dir (Contents/MacOS), so ../Frameworks lands on
# Contents/Frameworks from any loader. -add_rpath errors if it already exists;
# that is harmless, so the result is not checked.
file(GLOB _exes "${APP}/Contents/MacOS/*")
set(_rpath_targets ${_plugins})
foreach(_e IN LISTS _exes)
    if(NOT IS_DIRECTORY "${_e}")
        list(APPEND _rpath_targets "${_e}")
    endif()
endforeach()
foreach(_t IN LISTS _rpath_targets)
    execute_process(
        COMMAND install_name_tool -add_rpath "@executable_path/../Frameworks" "${_t}"
        RESULT_VARIABLE _ignored OUTPUT_QUIET ERROR_QUIET)
endforeach()

# ---- 3. Complete the non-Qt closure -----------------------------------------
# Walk libgdal's full dependency tree, copy every missing non-Qt/non-system
# library into Frameworks, and rewrite each reference to @executable_path/..
# /Frameworks. Pass Homebrew's lib dir(s) so deps referenced via @rpath resolve.
execute_process(
    COMMAND /bin/bash "${BUNDLE_SCRIPT}" "${APP}" "${HOMEBREW_LIB}" "${GDAL_LIB_DIR}"
    RESULT_VARIABLE _bl_rc)
if(NOT _bl_rc EQUAL 0)
    message(FATAL_ERROR "macos_bundle_libs.sh failed (exit ${_bl_rc})")
endif()

# ---- 4. Re-sign ------------------------------------------------------------
# The copies and install_name_tool rewrites invalidate every touched binary's
# code signature; AMFI (notably on Apple Silicon) then kills the process at
# launch. Re-sign the whole bundle ad-hoc and deep so every nested framework,
# bundled dylib and plugin is re-sealed. Ad-hoc (-s -) is enough to run locally;
# pass a real Developer ID (and drop --deep) for distribution. A signing failure
# is warned, not fatal.
execute_process(
    COMMAND codesign --force --deep --sign - "${APP}"
    RESULT_VARIABLE _cs_app OUTPUT_QUIET ERROR_QUIET)
if(NOT _cs_app EQUAL 0)
    message(WARNING "codesign (ad-hoc, --deep) failed for bundle: ${APP}")
endif()

message(STATUS "macdeploy: ${APP} is now self-contained (Qt + non-Qt closure bundled).")
