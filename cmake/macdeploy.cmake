# macOS bundle deployment — invoked from CMakeLists.txt via:
#
#   cmake -DAPP=<hmvchartplotter.app> \
#         -DMACDEPLOYQT=<macdeployqt> [-DDYLIBBUNDLER=<dylibbundler>] \
#         -DQT_LIB_DIR=<qt/lib> -DQT_LIB_DIR_REAL=<qt/lib realpath> \
#         -P cmake/macdeploy.cmake
#
# Makes the built .app self-contained (the macOS analogue of windeployqt): the
# Qt frameworks/plugins AND the non-Qt library closure (GDAL and its deps: GEOS,
# PROJ, SQLite, TIFF, OpenEXR, ...) are copied into the bundle and their
# references rewritten so it runs on a Mac without Homebrew/Qt installed.
#
# Two tools split the work, run in this order:
#   1. macdeployqt  — bundles the Qt frameworks + Qt platform/style plugins and
#      qt.conf (which dylibbundler can't), fixes our app plugins' Qt refs, and
#      makes a shallow, incomplete pass at libgdal's non-Qt deps.
#   2. dylibbundler — recurses the *non-Qt* closure and rewrites every reference
#      to @executable_path/../Frameworks (no rpath chains), completing what
#      macdeployqt left half-done.
# Order matters: macdeployqt re-copies libgdal's dependency tree from Homebrew
# incompletely, so it must run FIRST and dylibbundler LAST (it has the final say).
# dylibbundler is pointed only at the flat *.dylib files in Frameworks (the GDAL
# closure); Qt lives in *.framework directories, so Qt is never touched.

if(NOT EXISTS "${APP}")
    message(FATAL_ERROR "macdeploy: app bundle not found: ${APP}")
endif()
if(NOT MACDEPLOYQT)
    message(FATAL_ERROR "macdeploy: MACDEPLOYQT not set")
endif()

set(_frameworks "${APP}/Contents/Frameworks")

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

# ---- 2. Fix app-plugin rpaths ----------------------------------------------
# macdeployqt rewrites each -executable plugin's Qt references to @rpath/... but
# may not give those plugins an rpath to resolve it against (it reliably adds one
# only to the main exe). Anchor the rpath on @executable_path, which always
# resolves to the host binary's dir (Contents/MacOS) regardless of which file is
# doing the loading — so ../Frameworks lands on Contents/Frameworks from any
# plugin. -add_rpath errors if the entry already exists; that is harmless, so the
# result is not checked.
foreach(_p IN LISTS _plugins)
    execute_process(
        COMMAND install_name_tool -add_rpath "@executable_path/../Frameworks" "${_p}"
        RESULT_VARIABLE _ignored OUTPUT_QUIET ERROR_QUIET)
endforeach()

# ---- 3. Complete the non-Qt closure via dylibbundler ------------------------
# Point it at every flat *.dylib macdeployqt left in Frameworks (the GDAL closure;
# Qt is in *.framework dirs and is not matched). dylibbundler recurses each one,
# pulls in any still-missing transitive deps (e.g. libgeos, OpenEXR, aws-sdk), and
# rewrites all their references to @executable_path/../Frameworks. -s adds the
# Frameworks dir as a search path so @rpath deps resolve to already-bundled
# siblings; -i leaves the Qt lib dir alone; INPUT_FILE /dev/null makes an
# unresolved-library prompt fail fast instead of hanging the build.
if(DYLIBBUNDLER)
    file(GLOB _closure "${_frameworks}/*.dylib")
    if(_closure)
        set(_fix_args "")
        foreach(_lib IN LISTS _closure)
            list(APPEND _fix_args "-x" "${_lib}")
        endforeach()
        message(STATUS "dylibbundler: completing the non-Qt closure in ${_frameworks}")
        execute_process(
            COMMAND "${DYLIBBUNDLER}"
                    ${_fix_args}
                    -b            # copy and fix dependencies
                    -of           # overwrite copied files if already present
                    -cd           # create the destination dir if needed
                    -d "${_frameworks}"
                    -p "@executable_path/../Frameworks/"
                    -s "${_frameworks}"
                    -i "${QT_LIB_DIR}"
                    -i "${QT_LIB_DIR_REAL}"
            INPUT_FILE /dev/null
            RESULT_VARIABLE _db_rc)
        if(NOT _db_rc EQUAL 0)
            message(FATAL_ERROR
                "dylibbundler failed (exit ${_db_rc}). If it prompted for a missing "
                "library, add its directory as a search path (-s) in cmake/macdeploy.cmake.")
        endif()
    endif()
else()
    message(WARNING
        "dylibbundler not found — GDAL's dependency closure will NOT be fully "
        "bundled, and the app is likely to abort at launch with e.g. "
        "\"Library not loaded: @rpath/libgeos...\". Install it (brew install "
        "dylibbundler) and rebuild, or set -DCHARTPLOTTER_MACOS_DEPLOY=OFF and "
        "run against Homebrew's libraries during development.")
endif()

# ---- 3b. Normalise install IDs and mop up unfixed references ----------------
# dylibbundler rewrites the references it recognises, but leaves two classes
# behind in the copied libs:
#   * their own install id (LC_ID_DYLIB) still naming the original Homebrew path
#     — inert metadata, but it trips relocatability checks; and
#   * @rpath/<lib>.dylib dependency references (e.g. libgeos_c -> @rpath/
#     libgeos.3.14.1.dylib). These are BROKEN in the bundle: no rpath in the
#     load chain points at Contents/Frameworks, so dyld aborts at launch even
#     though the target file sits right there.
# Set every flat dylib's id to @executable_path/../Frameworks/<name>, then
# rewrite any absolute Homebrew/local reference AND any @rpath reference whose
# basename is bundled to the @executable_path form — making the closure's
# resolution fully explicit, with no dependence on rpaths at all.
file(GLOB _closure_fix "${_frameworks}/*.dylib")
foreach(_lib IN LISTS _closure_fix)
    get_filename_component(_base "${_lib}" NAME)
    execute_process(
        COMMAND install_name_tool -id "@executable_path/../Frameworks/${_base}" "${_lib}"
        OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND otool -L "${_lib}" OUTPUT_VARIABLE _deps ERROR_QUIET)
    # otool -L lines look like: <tab>/path/to/lib.dylib (compatibility ...).
    # These paths never contain spaces, so match a space-free run ending in
    # .dylib (the space before "(compatibility" bounds each match). Qt framework
    # refs (@rpath/QtCore.framework/...) don't end in .dylib, so Qt is untouched.
    string(REGEX MATCHALL "(/opt/homebrew|/usr/local|@rpath)[^ ]*\\.dylib" _refs "${_deps}")
    foreach(_dep IN LISTS _refs)
        get_filename_component(_dbase "${_dep}" NAME)
        if(EXISTS "${_frameworks}/${_dbase}")
            execute_process(
                COMMAND install_name_tool -change "${_dep}"
                        "@executable_path/../Frameworks/${_dbase}" "${_lib}"
                OUTPUT_QUIET ERROR_QUIET)
        endif()
    endforeach()
endforeach()

# Belt-and-braces: also give the host exe an rpath into the bundled Frameworks
# dir, so anything still resolved via @rpath (present or future) finds the
# bundled copies. macdeployqt does not reliably add this (observed: the exe kept
# only the build-time Homebrew Qt rpath, so @rpath lookups never searched
# Contents/Frameworks). -add_rpath errors if it already exists — harmless.
file(GLOB _exes "${APP}/Contents/MacOS/*")
foreach(_e IN LISTS _exes)
    if(NOT IS_DIRECTORY "${_e}")
        execute_process(
            COMMAND install_name_tool -add_rpath "@executable_path/../Frameworks" "${_e}"
            RESULT_VARIABLE _ignored OUTPUT_QUIET ERROR_QUIET)
    endif()
endforeach()

# ---- 4. Re-sign ------------------------------------------------------------
# dylibbundler and install_name_tool rewrite Mach-O headers, which invalidates
# every touched binary's code signature; AMFI (notably on Apple Silicon) then
# kills the process at launch. Re-sign the whole bundle ad-hoc and deep so every
# nested framework, bundled dylib and plugin is re-sealed. Ad-hoc (-s -) is
# enough to run locally; pass a real Developer ID (and drop --deep) for
# distribution. A signing failure is warned, not fatal.
execute_process(
    COMMAND codesign --force --deep --sign - "${APP}"
    RESULT_VARIABLE _cs_app OUTPUT_QUIET ERROR_QUIET)
if(NOT _cs_app EQUAL 0)
    message(WARNING "codesign (ad-hoc, --deep) failed for bundle: ${APP}")
endif()

message(STATUS "macdeploy: ${APP} is now self-contained (Qt + non-Qt closure bundled).")
