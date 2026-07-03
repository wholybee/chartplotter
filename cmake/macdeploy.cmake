# macOS bundle deployment — invoked from CMakeLists.txt via:
#
#   cmake -DAPP=<path/to/hmvchartplotter.app> \
#         -DMACDEPLOYQT=<path/to/macdeployqt> \
#         -P cmake/macdeploy.cmake
#
# Makes the built .app self-contained (the macOS analogue of windeployqt on
# Windows): macdeployqt copies the Qt frameworks and the Qt platform/style
# plugins into the bundle (Contents/Frameworks, Contents/PlugIns) and rewrites
# the host exe's framework references to @rpath so it runs on a Mac without Qt
# installed. This runs as a post-step after the exe AND every app plugin are in
# the bundle (see the APPLE block in CMakeLists.txt for the ordering).
#
# GDAL caveat: macdeployqt bundles Qt and makes a best-effort pass over other
# non-system dylibs the exe links (libgdal and its dependency closure). GDAL's
# tree is large and it dlopens some drivers, so verify a deployed build on a
# clean Mac (otool -L, and launch it) — you may still need to bundle GDAL's
# closure explicitly (e.g. dylibbundler) for full portability.

if(NOT EXISTS "${APP}")
    message(FATAL_ERROR "macdeploy: app bundle not found: ${APP}")
endif()
if(NOT MACDEPLOYQT)
    message(FATAL_ERROR "macdeploy: MACDEPLOYQT not set")
endif()

# The app's own plugins are copied into Contents/MacOS/plugins by each plugin
# target's post-build step (main.cpp loads applicationDirPath()/plugins). Pass
# each to macdeployqt with -executable= so their Qt references are rewritten to
# @rpath alongside the host exe's.
file(GLOB _plugins
    "${APP}/Contents/MacOS/plugins/*.so"
    "${APP}/Contents/MacOS/plugins/*.dylib")

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

# macdeployqt rewrites each -executable plugin's Qt references to @rpath/... but
# does not give those plugins an rpath to resolve it against (it only adds one to
# the main exe). Our plugins also sit a directory deeper than a standard bundle
# plugin (Contents/MacOS/plugins), so anchor the rpath on @executable_path, which
# always resolves to the host binary's dir (Contents/MacOS) regardless of which
# file is doing the loading — so ../Frameworks lands on Contents/Frameworks from
# any plugin. -add_rpath errors if the entry already exists; that is harmless, so
# the result is not checked.
foreach(_p IN LISTS _plugins)
    execute_process(
        COMMAND install_name_tool -add_rpath "@executable_path/../Frameworks" "${_p}"
        RESULT_VARIABLE _ignored OUTPUT_QUIET ERROR_QUIET)
endforeach()

# install_name_tool invalidates a Mach-O's code signature, so anything the AMFI
# loader checks (notably on Apple Silicon) would be killed at launch. Re-sign the
# modified plugins ad-hoc, then re-seal the bundle so its CodeResources manifest
# reflects the edited plugins. Ad-hoc (-s -) is enough to run locally; pass a real
# Developer ID here (or re-sign afterwards) for distribution. Signing failures are
# warned, not fatal, so the deploy still completes on unusual setups.
foreach(_p IN LISTS _plugins)
    execute_process(
        COMMAND codesign --force --sign - "${_p}"
        RESULT_VARIABLE _cs OUTPUT_QUIET ERROR_QUIET)
    if(NOT _cs EQUAL 0)
        message(WARNING "codesign (ad-hoc) failed for plugin: ${_p}")
    endif()
endforeach()

execute_process(
    COMMAND codesign --force --sign - "${APP}"
    RESULT_VARIABLE _cs_app OUTPUT_QUIET ERROR_QUIET)
if(NOT _cs_app EQUAL 0)
    message(WARNING "codesign (ad-hoc) failed for bundle: ${APP}")
endif()

message(STATUS "macdeploy: ${APP} is now self-contained (Qt bundled).")
