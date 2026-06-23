# GPX Plugin — built as a separate Qt MODULE library, loaded by the host at
# runtime via QPluginLoader. Mirrors plugins/signalk_plugin/CMakeLists.txt, but
# needs no extra Qt modules: GPX is parsed/written with QXmlStreamReader/Writer
# (Qt6::Core), and the only UI is built from Qt6::Widgets — both already deployed
# alongside the host exe, so there are no extra runtime DLLs to copy.

add_library(gpx_plugin MODULE
    gpx_plugin.hpp
    gpx_plugin.cpp
    gpx_plugin_factory.cpp
    gpx_io.hpp
    gpx_io.cpp
    gpx_plugin.json
)

set_target_properties(gpx_plugin PROPERTIES
    AUTOMOC ON
    # Stable basename so it never collides with the host or a third-party plugin
    # in the same plugins/ drop folder.
    OUTPUT_NAME "chartplotter_gpx_plugin"
    PREFIX ""           # no "lib" prefix on non-Windows MODULE libs
)

# Plugins consume the public chartplotter SDK headers and a small handful of
# core headers; they link against the host exe for those symbols (ENABLE_EXPORTS
# on chartviewer).
#
# Sanctioned core UI-helper headers a plugin may include (consumed by source,
# resolved against the host at link time): theme.hpp (palette), window_dragger.hpp
# (header-only; frameless-window drag) and dialog_chrome.hpp (header-only; the
# frameless side-menu dialog chrome — setup()/sectionHeader()/okCancelRow()/…,
# so plugin dialogs match the host without re-implementing the chrome). These are
# host-internal until the plugin SDK freezes an ABI; see docs/plugin_api.md.
target_include_directories(gpx_plugin PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(gpx_plugin PRIVATE
    Qt6::Widgets
    Qt6::Core
    Qt6::Sql           # route_store.hpp includes <QSqlDatabase> (headers only;
                       # Qt6Sql.dll is already deployed by the host)
    chartviewer        # host symbols (RouteStore, theme, route_types, ...)
)

# Deploy the built plugin next to the host exe so QPluginLoader finds it via the
# well-known plugins/ subfolder.
add_custom_command(TARGET gpx_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:chartviewer>/plugins"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:gpx_plugin>"
            "$<TARGET_FILE_DIR:chartviewer>/plugins/"
    COMMENT "Deploying gpx_plugin into plugins/ next to chartviewer"
    VERBATIM
)
