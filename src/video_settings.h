// Video options shared by the OpenGL renderer, the F4 menu, the launcher and the ini file.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>

#include "ini.h"

struct VideoSettings {
    // CRT effects, all in percent (0 = off)
    int scanlines = 0;
    int mask = 0;        // aperture grille / shadow mask
    int curvature = 0;   // screen bulge and rounded corners
    int glow = 0;        // bloom
    int vignette = 0;
    int persistence = 0; // phosphor afterglow: bright pixels fade out over a few frames
    int brightness = 100;  // 50..150
    int saturation = 100;  // 0..150
    int filter = 2;        // 0 sharp bilinear, 1 linear, 2 nearest
    int aspect = 0;        // 0 = 4:3 (arcade monitor), 1 = square pixels, 2 = stretch to the window
    int integer_scale = 0; // 1 = whole-number scale factor
    int bezel = 0;         // 1 = use the bezel image if one is found
    int preset = 0;        // index into presets(), 0 = custom (set when a value is changed by hand)

    // The values a preset sets. The user's own ("custom") values are kept separately, so picking a
    // preset and then "Custom" again brings them back.
    struct Look {
        int scanlines, mask, curvature, glow, vignette, persistence, brightness, saturation, filter, bezel;
    };
    Look custom{0, 0, 0, 0, 0, 0, 100, 100, 2, 0};

    struct Preset {
        const char* name;
        const char* label;
        Look look;
    };
    // index 0 is "custom" (its values are `custom`), the rest overwrite the effect values
    static const Preset* presets(int& count) {
        static const Preset p[] = {
            {"custom", "Custom", {0, 0, 0, 0, 0, 0, 100, 100, 0, 0}},
            {"off", "Off (clean pixels)", {0, 0, 0, 0, 0, 0, 100, 100, 2, 0}},
            {"scanlines", "Scanlines", {55, 0, 0, 12, 0, 0, 108, 100, 2, 0}},
            {"crt", "CRT", {50, 35, 20, 25, 25, 0, 118, 108, 0, 0}},
            {"crtstrong", "CRT strong", {75, 60, 35, 40, 40, 0, 128, 112, 0, 0}},
            {"crtbd", "CRT BlackDeath", {75, 60, 10, 40, 40, 0, 128, 112, 1, 1}},
        };
        count = (int)(sizeof(p) / sizeof(p[0]));
        return p;
    }

    Look look() const {
        return {scanlines, mask, curvature, glow, vignette, persistence, brightness, saturation, filter, bezel};
    }
    void set_look(const Look& l) {
        scanlines = l.scanlines; mask = l.mask; curvature = l.curvature; glow = l.glow; vignette = l.vignette;
        persistence = l.persistence; brightness = l.brightness; saturation = l.saturation; filter = l.filter; bezel = l.bezel;
    }

    // Selects preset `idx`; 0 restores the user's own values.
    void apply_preset(int idx) {
        int n;
        const Preset* p = presets(n);
        if (idx < 0 || idx >= n) return;
        if (preset == 0) custom = look();   // leaving "Custom": keep what the user set
        preset = idx;
        set_look(idx == 0 ? custom : p[idx].look);
    }
    bool apply_preset_name(const std::string& name) {
        int n;
        const Preset* p = presets(n);
        for (int i = 0; i < n; i++)
            if (name == p[i].name) { apply_preset(i); return true; }
        return false;
    }
    // A preset value was changed by hand: from now on these are the custom values.
    void changed_by_hand() {
        preset = 0;
        custom = look();
    }

    static constexpr const char* kFilters[3] = {"sharp", "linear", "nearest"};
    static constexpr const char* kAspects[3] = {"4:3", "pixel", "stretch"};

    void load(const Ini& ini) {
        scanlines = clamp(ini.get_int("video", "scanlines", scanlines), 0, 100);
        mask = clamp(ini.get_int("video", "mask", mask), 0, 100);
        curvature = clamp(ini.get_int("video", "curvature", curvature), 0, 100);
        glow = clamp(ini.get_int("video", "glow", glow), 0, 100);
        vignette = clamp(ini.get_int("video", "vignette", vignette), 0, 100);
        persistence = clamp(ini.get_int("video", "persistence", persistence), 0, 100);
        brightness = clamp(ini.get_int("video", "brightness", brightness), 50, 150);
        saturation = clamp(ini.get_int("video", "saturation", saturation), 0, 150);
        integer_scale = ini.get_bool("video", "integer_scale", integer_scale != 0) ? 1 : 0;
        bezel = ini.get_bool("video", "bezel", bezel != 0) ? 1 : 0;
        filter = filter_index(ini.get("video", "filter", kFilters[filter]), filter);
        const std::string a = ini.get("video", "aspect", kAspects[aspect]);
        for (int i = 0; i < 3; i++) if (a == kAspects[i]) aspect = i;
        const std::string pr = ini.get("video", "preset", "");
        int n;
        const Preset* p = presets(n);
        preset = 0;
        for (int i = 1; i < n; i++)
            if (pr == p[i].name) preset = i;
        // the user's own values: with "custom" active they are the current ones; otherwise from custom_*
        // (older files have none: then the current values)
        const Look cur = look();
        custom = cur;
        if (preset == 0) return;
        custom.scanlines = clamp(ini.get_int("video", "custom_scanlines", cur.scanlines), 0, 100);
        custom.mask = clamp(ini.get_int("video", "custom_mask", cur.mask), 0, 100);
        custom.curvature = clamp(ini.get_int("video", "custom_curvature", cur.curvature), 0, 100);
        custom.glow = clamp(ini.get_int("video", "custom_glow", cur.glow), 0, 100);
        custom.vignette = clamp(ini.get_int("video", "custom_vignette", cur.vignette), 0, 100);
        custom.persistence = clamp(ini.get_int("video", "custom_persistence", cur.persistence), 0, 100);
        custom.brightness = clamp(ini.get_int("video", "custom_brightness", cur.brightness), 50, 150);
        custom.saturation = clamp(ini.get_int("video", "custom_saturation", cur.saturation), 0, 150);
        custom.filter = filter_index(ini.get("video", "custom_filter", kFilters[cur.filter]), cur.filter);
        custom.bezel = ini.get_bool("video", "custom_bezel", cur.bezel != 0) ? 1 : 0;
    }
    void save(Ini& ini) const {
        int n;
        const Preset* p = presets(n);
        ini.set("video", "preset", p[preset].name);
        ini.set_int("video", "scanlines", scanlines);
        ini.set_int("video", "mask", mask);
        ini.set_int("video", "curvature", curvature);
        ini.set_int("video", "glow", glow);
        ini.set_int("video", "vignette", vignette);
        ini.set_int("video", "persistence", persistence);
        ini.set_int("video", "brightness", brightness);
        ini.set_int("video", "saturation", saturation);
        ini.set("video", "filter", kFilters[filter]);
        ini.set("video", "aspect", kAspects[aspect]);
        ini.set_bool("video", "integer_scale", integer_scale != 0);
        ini.set_bool("video", "bezel", bezel != 0);
        const Look c = preset == 0 ? look() : custom;
        ini.set_int("video", "custom_scanlines", c.scanlines);
        ini.set_int("video", "custom_mask", c.mask);
        ini.set_int("video", "custom_curvature", c.curvature);
        ini.set_int("video", "custom_glow", c.glow);
        ini.set_int("video", "custom_vignette", c.vignette);
        ini.set_int("video", "custom_persistence", c.persistence);
        ini.set_int("video", "custom_brightness", c.brightness);
        ini.set_int("video", "custom_saturation", c.saturation);
        ini.set("video", "custom_filter", kFilters[std::max(0, std::min(2, c.filter))]);
        ini.set_bool("video", "custom_bezel", c.bezel != 0);
    }

    static int clamp(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
    static int filter_index(const std::string& f, int def) {   // older files: nearest / linear
        for (int i = 0; i < 3; i++) if (f == kFilters[i]) return i;
        return def;
    }
};
