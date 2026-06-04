#pragma once
#include <glib.h>
#include <glib/gstdio.h>
#include <string>

// Minimal persistent configuration, stored as an INI file in the user's config
// directory (e.g. %APPDATA% on Windows, ~/.config on Linux). We remember the
// last chart folder so the app reopens it on launch.

inline std::string settings_path() {
    char* dir = g_build_filename(g_get_user_config_dir(), "marine-chart-viewer", nullptr);
    g_mkdir_with_parents(dir, 0700);
    char* file = g_build_filename(dir, "settings.ini", nullptr);
    std::string result = file ? file : "";
    g_free(dir);
    g_free(file);
    return result;
}

inline std::string settings_load_dir() {
    std::string result;
    GKeyFile* kf = g_key_file_new();
    std::string path = settings_path();
    if (g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
        char* v = g_key_file_get_string(kf, "charts", "directory", nullptr);
        if (v) { result = v; g_free(v); }
    }
    g_key_file_free(kf);
    return result;
}

inline void settings_save_dir(const std::string& dir) {
    GKeyFile* kf = g_key_file_new();
    std::string path = settings_path();
    g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr); // ignore failure
    g_key_file_set_string(kf, "charts", "directory", dir.c_str());

    gsize len = 0;
    char* data = g_key_file_to_data(kf, &len, nullptr);
    if (data) {
        g_file_set_contents(path.c_str(), data, static_cast<gssize>(len), nullptr);
        g_free(data);
    }
    g_key_file_free(kf);
}
