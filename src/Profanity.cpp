#include "Profanity.h"
#include <cstring>
#include <cstdint>
#include <vector>

namespace mc {

static bool is_ascii_upper(unsigned char c) { return c >= 'A' && c <= 'Z'; }

std::string normalize_word(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        unsigned char c = (unsigned char)in[i];
        if (is_ascii_upper(c)) { out += (char)(c + 32); continue; }
        if (c == 0xD0 && i + 1 < in.size()) {
            unsigned char d = (unsigned char)in[i + 1];
            if (d == 0x81) { out += (char)0xD0; out += (char)0xB5; i++; continue; } // Ё -> е
            if (d >= 0x90 && d <= 0x9F) { out += (char)0xD0; out += (char)(d + 0x20); i++; continue; } // А-П
            if (d >= 0xA0 && d <= 0xAF) { out += (char)0xD1; out += (char)(d - 0x20); i++; continue; } // Р-Я
        }
        if (c == 0xD1 && i + 1 < in.size()) {
            unsigned char d = (unsigned char)in[i + 1];
            if (d == 0x91) { out += (char)0xD0; out += (char)0xB5; i++; continue; } // ё -> е
        }
        out += (char)c;
    }
    return out;
}

// Decode one UTF-8 codepoint at in[i], advance i past it.
static uint32_t next_cp(const std::string& in, size_t& i) {
    unsigned char c = (unsigned char)in[i];
    uint32_t cp = c;
    int extra = 0;
    if (c >= 0xF0) { cp = c & 7; extra = 3; }
    else if (c >= 0xE0) { cp = c & 15; extra = 2; }
    else if (c >= 0xC0) { cp = c & 31; extra = 1; }
    i++;
    for (int k = 0; k < extra && i < in.size(); k++, i++)
        cp = (cp << 6) | ((unsigned char)in[i] & 0x3F);
    return cp;
}

static bool is_punct_cp(uint32_t cp) {
    if (cp < 128) return std::strchr(" \t\r\n.,!?;:\"'()[]{}<>/\\|@#$%^&*+=~`-", (int)cp) != nullptr;
    switch (cp) {
    case 0x00AB: case 0x00BB: case 0x2013: case 0x2014: case 0x2026:
    case 0x201C: case 0x201D: case 0x2018: case 0x2019: case 0x3000:
        return true;
    default: return false;
    }
}

std::string trim_punct(const std::string& in) {
    // Collect codepoint start offsets, then trim punctuation from both ends.
    std::vector<size_t> starts;
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < in.size();) {
        starts.push_back(i);
        cps.push_back(next_cp(in, i));
    }
    size_t a = 0, b = cps.size();
    while (a < b && is_punct_cp(cps[a])) a++;
    while (b > a && is_punct_cp(cps[b - 1])) b--;
    if (a >= b) return "";
    size_t end_byte = (b < starts.size()) ? starts[b] : in.size();
    return in.substr(starts[a], end_byte - starts[a]);
}

void Dictionary::add(const std::string& pattern, std::vector<std::string> wl, int severity) {
    entries.push_back({normalize_word(pattern), {}, severity});
    for (auto& w : wl) entries.back().whitelist.push_back(normalize_word(w));
}

std::string Dictionary::match(const std::string& word) const {
    for (const auto& w : global_whitelist)
        if (!w.empty() && word.find(w) != std::string::npos) return "";
    for (const auto& e : entries) {
        if (e.severity > max_severity) continue;
        if (e.pattern.empty() || word.find(e.pattern) == std::string::npos) continue;
        bool blocked = false;
        for (const auto& w : e.whitelist)
            if (!w.empty() && word.find(w) != std::string::npos) { blocked = true; break; }
        if (!blocked) return e.pattern;
    }
    return "";
}

Dictionary default_dictionary() {
    Dictionary d;
    // Severity 1: severe (core mat, sexual/scatological, slurs).
    d.add("хуй", {}, 1); d.add("хуе", {}, 1); d.add("хуя", {}, 1); d.add("хую", {}, 1); d.add("хуи", {}, 1);
    d.add("пизд", {}, 1); d.add("пизж", {}, 1);
    d.add("бля", {"бляш"}, 1);
    d.add("еб", {"себ","теб","неб","треб","реб","леб","чеб","деб","преб","шеб","жеб","греб","хлеб","щеб","геб"}, 1);
    d.add("залуп", {}, 1);
    d.add("пидор", {}, 1); d.add("пидар", {}, 1);
    d.add("гандон", {}, 1); d.add("гондон", {}, 1);
    d.add("елд", {}, 1);
    d.add("манда", {"мандат","мандарин","мандолин","мандибул"}, 1);
    d.add("шлюх", {}, 1);
    d.add("fuck", {}, 1); d.add("cunt", {}, 1);
    d.add("nigger", {}, 1); d.add("nigga", {}, 1); d.add("faggot", {}, 1);
    d.add("cock", {"cockpit","cockney","cocktail","peacock","hancock"}, 1);
    d.add("pussy", {}, 1); d.add("blowjob", {}, 1); d.add("handjob", {}, 1); d.add("rimjob", {}, 1);
    d.add("asshole", {}, 1);
    // Severity 2: medium profanity.
    d.add("дроч", {}, 2);
    d.add("говн", {}, 2); d.add("гавн", {}, 2);
    d.add("муд", {"мудр","изумруд"}, 2);
    d.add("хер", {"херес","херувим","херсон","махер","хертель"}, 2);
    d.add("срак", {}, 2); d.add("срат", {}, 2); d.add("сран", {}, 2); d.add("срач", {}, 2);
    d.add("ссан", {}, 2); d.add("ссык", {}, 2); d.add("ссат", {}, 2);
    d.add("сук", {"сукно","сукровиц","суккулент","сукцесс","сукон"}, 2);
    d.add("суч", {"сучок","сучков","сучь"}, 2);
    d.add("shit", {}, 2); d.add("bitch", {}, 2);
    d.add("dick", {"dickens"}, 2);
    d.add("whore", {}, 2); d.add("slut", {}, 2); d.add("wanker", {}, 2);
    d.add("bastard", {}, 2); d.add("twat", {}, 2); d.add("bollock", {}, 2);
    d.add("jackass", {}, 2); d.add("dumbass", {}, 2); d.add("fag", {"fagan"}, 2);
    // Severity 3: mild / borderline.
    d.add("жоп", {}, 3);
    d.add("дерьм", {}, 3); d.add("задниц", {}, 3);
    d.add("damn", {}, 3); d.add("crap", {}, 3);
    return d;
}

} // namespace mc