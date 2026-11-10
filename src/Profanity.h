// Profanity dictionary: Russian mat + English, UTF-8 Cyrillic aware.
// Severity tiers: 1 = severe (core mat / slurs), 2 = medium, 3 = mild/borderline.
#pragma once
#include <string>
#include <vector>

namespace mc {

std::string normalize_word(const std::string& in);
std::string trim_punct(const std::string& in);

struct DictEntry {
    std::string pattern;
    std::vector<std::string> whitelist;
    int severity = 1;
};

struct Dictionary {
    std::vector<DictEntry> entries;
    std::vector<std::string> global_whitelist;
    int max_severity = 3;   // entries with severity > max_severity are ignored
    void add(const std::string& pattern, std::vector<std::string> wl = {}, int severity = 1);
    std::string match(const std::string& word_normalized) const;
};

Dictionary default_dictionary();

} // namespace mc