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
    std::string replacement_sound; // WAV path for "Custom sound" replacement
    std::vector<std::string> custom_words;
    std::vector<std::string> whitelist;
};

// Search <plugin_dir>/MatCutter.json then D:/AEplugin/MatCutter.json.
Config load_config(const std::string& plugin_dir);

// model_choice: 1 tiny, 2 base, 3 small, 4 medium, 5 large-v3, 6 from config.
std::string resolve_model_path(const Config& cfg, int model_choice);

} // namespace mc