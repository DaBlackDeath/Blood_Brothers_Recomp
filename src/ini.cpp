#include "ini.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace {
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
}  // namespace

Ini::Section* Ini::find(const std::string& name) {
    for (auto& s : sections_)
        if (s.name == name) return &s;
    return nullptr;
}
const Ini::Section* Ini::find(const std::string& name) const {
    for (auto& s : sections_)
        if (s.name == name) return &s;
    return nullptr;
}

bool Ini::load(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);

    sections_.clear();
    std::string cur;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(pos, e - pos));
        pos = e + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') {
            cur = trim(line.substr(1, line.size() - 2));
            if (!find(cur)) sections_.push_back({cur, {}});
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        set(cur, trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
    return true;
}

bool Ini::save(const std::string& path, const std::string& header) const {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    if (!header.empty()) std::fprintf(f, "%s\r\n", header.c_str());
    for (const auto& s : sections_) {
        if (!s.name.empty()) std::fprintf(f, "\r\n[%s]\r\n", s.name.c_str());
        for (const auto& kv : s.kv) std::fprintf(f, "%s=%s\r\n", kv.first.c_str(), kv.second.c_str());
    }
    std::fclose(f);
    return true;
}

bool Ini::has(const std::string& section, const std::string& key) const {
    const Section* s = find(section);
    if (!s) return false;
    for (const auto& kv : s->kv)
        if (kv.first == key) return true;
    return false;
}

std::string Ini::get(const std::string& section, const std::string& key, const std::string& def) const {
    const Section* s = find(section);
    if (s)
        for (const auto& kv : s->kv)
            if (kv.first == key) return kv.second;
    return def;
}

int Ini::get_int(const std::string& section, const std::string& key, int def) const {
    const std::string v = get(section, key);
    if (v.empty()) return def;
    char* end = nullptr;
    const long x = std::strtol(v.c_str(), &end, 10);
    return end == v.c_str() ? def : (int)x;
}

bool Ini::get_bool(const std::string& section, const std::string& key, bool def) const {
    const std::string v = get(section, key);
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

void Ini::set(const std::string& section, const std::string& key, const std::string& value) {
    Section* s = find(section);
    if (!s) {
        sections_.push_back({section, {}});
        s = &sections_.back();
    }
    for (auto& kv : s->kv)
        if (kv.first == key) { kv.second = value; return; }
    s->kv.push_back({key, value});
}

void Ini::clear_section(const std::string& section) {
    if (Section* s = find(section)) s->kv.clear();
}

std::vector<std::pair<std::string, std::string>> Ini::items(const std::string& section) const {
    const Section* s = find(section);
    return s ? s->kv : std::vector<std::pair<std::string, std::string>>();
}
