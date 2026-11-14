// JSON config: model paths, Groq key, custom words / whitelist.
#pragma once
#include <string>
#include <vector>

namespace mc {

struct Config {
    std::string models_dir;
    std::string model_file;    // explicit path for "from config" model choice
    std::string groq_api_key;  // single key (legacy)
    std::vector<std::string> groq_api_keys; // key pool (rotated)
    std::string proxy;         // HTTP proxy for Groq, e.g. http://127.0.0.1:7890
    std::string replacement_sound; // WAV path for "Custom sound" replacement
    std::vector<std::string> custom_words;
    std::vector<std::string> whitelist;
};

// Search <plugin_dir>/NwRemover.json then D:/AEplugin/NwRemover.json.
Config load_config(const std::string& plugin_dir);

// Append a Groq key to groq_api_keys in the config file (dedupe).
// Uses the same search order as load_config; creates the file if missing.
bool add_groq_key(const std::string& plugin_dir, const std::string& key);

// Set models_dir in the config file (same search order as load_config).
bool set_models_dir(const std::string& plugin_dir, const std::string& dir);

// Set replacement_sound (WAV path) in the config file (same search order).
bool set_replacement_sound(const std::string& plugin_dir, const std::string& path);

// model_choice: 1 tiny, 2 base, 3 small, 4 medium, 5 large-v3, 6 from config.
std::string resolve_model_path(const Config& cfg, int model_choice);

} // namespace mc