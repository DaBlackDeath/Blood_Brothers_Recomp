// Tiny INI file reader/writer ([section] / key=value, ';' or '#' comments). Order is kept.
#pragma once

#include <string>
#include <utility>
#include <vector>

class Ini {
public:
    bool load(const std::string& path);
    bool save(const std::string& path, const std::string& header = std::string()) const;

    bool has(const std::string& section, const std::string& key) const;
    std::string get(const std::string& section, const std::string& key, const std::string& def = std::string()) const;
    int get_int(const std::string& section, const std::string& key, int def) const;
    bool get_bool(const std::string& section, const std::string& key, bool def) const;
    void set(const std::string& section, const std::string& key, const std::string& value);
    void set_int(const std::string& section, const std::string& key, int value) { set(section, key, std::to_string(value)); }
    void set_bool(const std::string& section, const std::string& key, bool value) { set(section, key, value ? "1" : "0"); }
    void clear_section(const std::string& section);
    std::vector<std::pair<std::string, std::string>> items(const std::string& section) const;

private:
    struct Section {
        std::string name;
        std::vector<std::pair<std::string, std::string>> kv;
    };
    Section* find(const std::string& name);
    const Section* find(const std::string& name) const;
    std::vector<Section> sections_;
};
