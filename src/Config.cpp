#include "Config.h"
#include "json.hpp"
#include <fstream>
#include <sstream>
#include <windows.h>

namespace mc {

static bool file_exists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool dir_exists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a[a.size() - 1];
    return a + ((last == '/' || last == '\\') ? "" : "/") + b;
}

static std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

Config load_config(const std::string& plugin_dir) {
    Config cfg;
    std::string body = read_file(join(plugin_dir, "NwRemover.json"));
    if (body.empty()) body = read_file("D:/AEplugin/NwRemover.json");
    if (!body.empty()) {
        auto j = nlohmann::json::parse(body, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            if (j.contains("models_dir") && j["models_dir"].is_string()) cfg.models_dir = j["models_dir"].get<std::string>();
            if (j.contains("model_file") && j["model_file"].is_string()) cfg.model_file = j["model_file"].get<std::string>();
            if (j.contains("groq_api_key") && j["groq_api_key"].is_string()) cfg.groq_api_key = j["groq_api_key"].get<std::string>();
            if (j.contains("groq_api_keys") && j["groq_api_keys"].is_array())
                for (auto& v : j["groq_api_keys"]) if (v.is_string() && !v.get<std::string>().empty()) cfg.groq_api_keys.push_back(v.get<std::string>());
            if (j.contains("replacement_sound") && j["replacement_sound"].is_string()) cfg.replacement_sound = j["replacement_sound"].get<std::string>();
            if (j.contains("proxy") && j["proxy"].is_string()) cfg.proxy = j["proxy"].get<std::string>();
            if (j.contains("custom_words") && j["custom_words"].is_array())
                for (auto& v : j["custom_words"]) if (v.is_string()) cfg.custom_words.push_back(v.get<std::string>());
            if (j.contains("whitelist") && j["whitelist"].is_array())
                for (auto& v : j["whitelist"]) if (v.is_string()) cfg.whitelist.push_back(v.get<std::string>());
        }
    }
    if (cfg.models_dir.empty()) {
        std::string local = join(plugin_dir, "models");
        cfg.models_dir = dir_exists(local) ? local : "D:/AEplugin/models";
    }
    return cfg;
}

std::string resolve_model_path(const Config& cfg, int model_choice) {
    const char* names[] = {"", "ggml-tiny.bin", "ggml-base.bin", "ggml-small.bin",
                           "ggml-medium.bin", "ggml-large-v3.bin"};
    if (model_choice == 6) {
        if (!cfg.model_file.empty()) return cfg.model_file;
        return join(cfg.models_dir, "ggml-base.bin");
    }
    if (model_choice < 1 || model_choice > 5) model_choice = 2;
    return join(cfg.models_dir, names[model_choice]);
}

bool add_groq_key(const std::string& plugin_dir, const std::string& key) {
    std::string p1 = join(plugin_dir, "NwRemover.json");
    std::string p2 = "D:/AEplugin/NwRemover.json";
    std::string path = file_exists(p1) ? p1 : p2;
    nlohmann::json j = nlohmann::json::object();
    std::string body = read_file(path);
    if (!body.empty()) {
        auto parsed = nlohmann::json::parse(body, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object()) j = parsed;
    }
    auto& arr = j["groq_api_keys"];
    if (!arr.is_array()) arr = nlohmann::json::array();
    for (auto& v : arr)
        if (v.is_string() && v.get<std::string>() == key) return true; // already present
    arr.push_back(key);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return (bool)f;
}

bool set_models_dir(const std::string& plugin_dir, const std::string& dir) {
    std::string p1 = join(plugin_dir, "NwRemover.json");
    std::string p2 = "D:/AEplugin/NwRemover.json";
    std::string path = file_exists(p1) ? p1 : p2;
    nlohmann::json j = nlohmann::json::object();
    std::string body = read_file(path);
    if (!body.empty()) {
        auto parsed = nlohmann::json::parse(body, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object()) j = parsed;
    }
    j["models_dir"] = dir;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return (bool)f;
}

bool set_replacement_sound(const std::string& plugin_dir, const std::string& path) {
    std::string p1 = join(plugin_dir, "NwRemover.json");
    std::string p2 = "D:/AEplugin/NwRemover.json";
    std::string p = file_exists(p1) ? p1 : p2;
    nlohmann::json j = nlohmann::json::object();
    std::string body = read_file(p);
    if (!body.empty()) {
        auto parsed = nlohmann::json::parse(body, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object()) j = parsed;
    }
    j["replacement_sound"] = path;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return (bool)f;
}

} // namespace mc