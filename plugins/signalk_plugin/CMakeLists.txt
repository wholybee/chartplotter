# Signal K Plugin — built as a separate Qt MODULE library, loaded by the host
# at runtime via QPluginLoader. Mirrors plugins/test_plugin/CMakeLists.txt, but
# also links Qt6::WebSockets (for the ws:// transport) and Qt6::Network.

add_library(signalk_plugin MODULE
    signalk_plugin.hpp
    signalk_plugin.cpp
    signalk_plugin_factory.cpp
    signalk_client.hpp
    signalk_client.cpp
    signalk_decoder.hpp
    signalk_decoder.cpp
    signalk_plugin.json
)

set_target_properties(signalk_plugin PROPERTIES
    AUTOMOC ON
    # Stable basename so it never collides with the host or a third-party plugin
    # in the same plugins/ drop folder.
    OUTPUT_NAME "chartplotter_signalk_plugin"
    PREFIX ""           # no "lib" prefix on non-Windows MODULE libs
)

# Plugins consume the public chartplotter SDK headers and a small handful of
# core headers; they link against the host exe for those symbols (ENABLE_EXPORTS
# on chartviewer).
target_include_directories(signalk_plugin PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(signalk_plugin PRIVATE
    Qt6::Widgets
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    chartviewer        # host symbols (NavDataStore, theme, TouchSpinBox, ...)
)

# Deploy into the host's plugin folder (plugins/ next to the exe, or
# Contents/PlugIns on macOS) so QPluginLoader finds it at runtime.
add_custom_command(TARGET signalk_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CHARTPLOTTER_PLUGIN_DEPLOY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:signalk_plugin>"
            "${CHARTPLOTTER_PLUGIN_DEPLOY_DIR}/"
    COMMENT "Deploying signalk_plugin into the host plugin folder"
    VERBATIM
)

# windeployqt is invoked against chartviewer.exe, which doesn't link Qt6Network /
# Qt6WebSockets directly (only this plugin does), so those DLLs are not copied
# automatically. Drop them next to the exe ourselves so QPluginLoader can resolve
# the plugin's imports at runtime. On macOS this is skipped: macdeployqt bundles
# the frameworks (the plugin is passed to it via -executable), and copying a
# framework binary into Contents/MacOS would break code signing.
if(NOT APPLE)
    add_custom_command(TARGET signalk_plugin POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::WebSockets>"
                "$<TARGET_FILE_DIR:chartviewer>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::Network>"
                "$<TARGET_FILE_DIR:chartviewer>"
        COMMENT "Copying Qt6WebSockets/Qt6Network libraries next to chartviewer"
        VERBATIM
    )
endif()
