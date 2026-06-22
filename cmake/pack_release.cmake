# Invoked (via `cmake -P`) by the `installer` custom target to build the NSIS
# installer as part of a Release build. It is a no-op for any other
# configuration, because the install payload (install(DIRECTORY .../Release/ ...))
# is staged for Release only — running cpack on a Debug build would package
# nothing useful (or fail).
#
# Inputs (passed with -D by the target):
#   CFG          the build configuration ($<CONFIG>)
#   CPACK_CMD    full path to the cpack executable (CMAKE_CPACK_COMMAND)
#   CPACK_CONFIG path to the generated CPackConfig.cmake
#   BIN_DIR      the build directory (working dir for cpack)

if(NOT CFG STREQUAL "Release")
    message(STATUS "[installer] skipping NSIS packaging for ${CFG} build (Release only)")
    return()
endif()

message(STATUS "[installer] building NSIS installer ...")
execute_process(
    COMMAND "${CPACK_CMD}" --config "${CPACK_CONFIG}" -C Release -G NSIS
    WORKING_DIRECTORY "${BIN_DIR}"
    RESULT_VARIABLE _cpack_rv
)
if(NOT _cpack_rv EQUAL 0)
    message(FATAL_ERROR
        "[installer] cpack failed (exit ${_cpack_rv}). Is NSIS (makensis) on PATH?")
endif()
message(STATUS "[installer] installer written under ${BIN_DIR}/installer")
