#include "menu.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "dips.h"
#include "font8x8_basic.h"

namespace {

constexpr uint32_t kPanel = 0xE0101420, kBorder = 0xFF6070B0, kText = 0xFFE0E0E0, kDimText = 0xFF8088A0;
constexpr uint32_t kSelBg = 0xFF2858B8, kTitle = 0xFFFFE060, kHelp = 0xFFA0C8FF, kOn = 0xFF80FF90, kWarn = 0xFFFF9060;

struct Canvas {
    uint32_t* px;
    int w, h;
    void rect(int x, int y, int rw, int rh, uint32_t c) {
        for (int j = std::max(0, y); j < std::min(h, y + rh); j++)
            for (int i = std::max(0, x); i < std::min(w, x + rw); i++) px[j * w + i] = c;
    }
    void glyph(int x, int y, unsigned char ch, uint32_t c) {
        if (ch > 127) ch = '?';
        const unsigned char* g = font8x8_basic[ch];
        for (int j = 0; j < 8; j++)
            for (int i = 0; i < 8; i++)
                if ((g[j] >> i) & 1) {
                    const int X = x + i, Y = y + j;
                    if (X >= 0 && X < w && Y >= 0 && Y < h) px[Y * w + X] = c;
                }
    }
    void text(int x, int y, const std::string& s, uint32_t c) {
        for (size_t i = 0; i < s.size(); i++) glyph(x + (int)i * 8, y, (unsigned char)s[i], c);
    }
};

std::string clip(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n - 1) + "~"; }

// word-wrap `s` into lines of at most `n` characters
std::vector<std::string> wrap(const std::string& s, size_t n) {
    std::vector<std::string> out;
    std::string line, word;
    auto flush_word = [&]() {
        if (word.empty()) return;
        if (!line.empty() && line.size() + 1 + word.size() > n) { out.push_back(line); line.clear(); }
        line += (line.empty() ? "" : " ") + word;
        word.clear();
    };
    for (char c : s) {
        if (c == ' ') flush_word();
        else word += c;
    }
    flush_word();
    if (!line.empty()) out.push_back(line);
    return out;
}

struct VParam {
    const char* name;
    int VideoSettings::* field;
    int lo, hi, step;
    const char* const* labels;
    int nlabels;
};
const char* const kOnOff[] = {"Off", "On"};
const char* const kFilterLabels[] = {"Sharp bilinear", "Linear", "Nearest"};
const char* const kAspectLabels[] = {"4:3 (arcade monitor)", "Square pixels", "Stretch to window"};
const VParam kVParams[] = {
    {"Scanlines", &VideoSettings::scanlines, 0, 100, 5, nullptr, 0},
    {"Shadow mask", &VideoSettings::mask, 0, 100, 5, nullptr, 0},
    {"Screen curvature", &VideoSettings::curvature, 0, 100, 5, nullptr, 0},
    {"Glow", &VideoSettings::glow, 0, 100, 5, nullptr, 0},
    {"Vignette", &VideoSettings::vignette, 0, 100, 5, nullptr, 0},
    {"Phosphor afterglow", &VideoSettings::persistence, 0, 100, 5, nullptr, 0},
    {"Brightness", &VideoSettings::brightness, 50, 150, 5, nullptr, 0},
    {"Saturation", &VideoSettings::saturation, 0, 150, 5, nullptr, 0},
    {"Pixel filter", &VideoSettings::filter, 0, 2, 1, kFilterLabels, 3},
    {"Aspect ratio", &VideoSettings::aspect, 0, 2, 1, kAspectLabels, 3},
    {"Integer scaling", &VideoSettings::integer_scale, 0, 1, 1, kOnOff, 2},
    {"Bezel (monitor frame)", &VideoSettings::bezel, 0, 1, 1, kOnOff, 2},
};
constexpr int kVParamCount = (int)(sizeof(kVParams) / sizeof(kVParams[0]));
constexpr int kPresetRef = 100;

std::string video_value(const VideoSettings& v, const VParam& p) {
    const int x = v.*(p.field);
    if (p.labels) return p.labels[std::max(0, std::min(x, p.nlabels - 1))];
    return std::to_string(x) + "%";
}

const char* dip_label(const DipDef& d, uint16_t dsw) {
    for (int i = 0; i < d.count; i++)
        if (d.opts[i].value == (dsw & d.mask)) return d.opts[i].label;
    return "?";
}

bool dip_visible(const DipDef& d, uint16_t dsw) { return d.cond_mask == 0 || (dsw & d.cond_mask) == d.cond_value; }

}  // namespace

void Menu::open(Page p) {
    page_ = p;
    sel_ = 0;
    top_ = 0;
    build_rows();
    // start on the first selectable row
    if (!rows_.empty() && rows_[0].kind != 1 && rows_[0].kind != 2) sel_ = move(0, 1);
}

int Menu::move(int from, int dir) const {
    const int n = (int)rows_.size();
    for (int step = 1; step <= n; step++) {
        const int i = ((from + dir * step) % n + n) % n;
        if (rows_[(size_t)i].kind == 1 || rows_[(size_t)i].kind == 2) return i;
    }
    return from;
}

void Menu::build_rows() {
    rows_.clear();
    if (page_ == Page::Dip) {
        const uint16_t dsw = hooks_.get_dsw ? hooks_.get_dsw() : kDipDefault;
        for (int i = 0; i < kDipCount; i++) {
            const DipDef& d = kDips[i];
            if (!dip_visible(d, dsw)) continue;
            Row r;
            char buf[64];
            std::snprintf(buf, sizeof buf, "%-8s %s", d.loc, d.name);
            r.left = buf;
            r.right = dip_label(d, dsw);
            r.kind = 1;
            r.ref = i;
            r.dim = std::strcmp(d.name, "Unused") == 0;
            rows_.push_back(r);
        }
        rows_.push_back({});
        rows_.push_back({"Restore factory settings", "", 2, -2, false});
        rows_.push_back({"Reset game now (apply changes)", "", 2, -3, false});
    } else if (page_ == Page::Video) {
        if (!hooks_.video) return;
        const VideoSettings& v = *hooks_.video;
        int np;
        const VideoSettings::Preset* pr = VideoSettings::presets(np);
        rows_.push_back({"Preset", pr[std::max(0, std::min(v.preset, np - 1))].label, 1, kPresetRef, false});
        rows_.push_back({});
        for (int i = 0; i < kVParamCount; i++) {
            Row r;
            r.left = kVParams[i].name;
            r.right = video_value(v, kVParams[i]);
            r.kind = 1;
            r.ref = i;
            rows_.push_back(r);
        }
        rows_.push_back({});
        rows_.push_back({"Restore video defaults", "", 2, -5, false});
    } else if (page_ == Page::Cheats) {
        if (!hooks_.cheats || hooks_.cheats->cheats().empty()) {
            rows_.push_back({hooks_.cheat_file_note.empty() ? "No cheat file loaded." : hooks_.cheat_file_note, "", 3, -1, true});
            return;
        }
        auto& cs = hooks_.cheats->cheats();
        for (size_t i = 0; i < cs.size(); i++) {
            const cheat::Cheat& c = cs[i];
            Row r;
            if (c.separator) { rows_.push_back(r); continue; }
            r.left = std::string(c.enabled ? "[x] " : "[ ] ") + c.desc;
            if (!c.error.empty()) { r.left = "[!] " + c.desc; r.dim = true; r.kind = 3; }
            else {
                r.kind = 1;
                if (!c.items.empty()) r.right = c.items[(size_t)std::min<int>(c.param_index, (int)c.items.size() - 1)].label;
            }
            r.ref = (int)i;
            rows_.push_back(r);
        }
        rows_.push_back({});
        rows_.push_back({"Switch all cheats off", "", 2, -4, false});
    }
}

void Menu::change(int dir) {
    if (sel_ < 0 || sel_ >= (int)rows_.size()) return;
    const Row& r = rows_[(size_t)sel_];
    if (r.kind != 1) return;
    if (page_ == Page::Dip) {
        const DipDef& d = kDips[r.ref];
        uint16_t dsw = hooks_.get_dsw ? hooks_.get_dsw() : kDipDefault;
        int cur = 0;
        for (int i = 0; i < d.count; i++)
            if (d.opts[i].value == (dsw & d.mask)) cur = i;
        cur = ((cur + dir) % d.count + d.count) % d.count;
        dsw = (uint16_t)((dsw & ~d.mask) | d.opts[cur].value);
        if (hooks_.set_dsw) hooks_.set_dsw(dsw);
        if (hooks_.settings_changed) hooks_.settings_changed();
        const int keep = r.ref;
        build_rows();  // other entries (coinage) may appear or disappear
        for (int i = 0; i < (int)rows_.size(); i++)
            if (rows_[(size_t)i].ref == keep && rows_[(size_t)i].kind == 1) sel_ = i;
    } else if (page_ == Page::Video && hooks_.video) {
        VideoSettings& v = *hooks_.video;
        if (r.ref == kPresetRef) {
            int np;
            VideoSettings::presets(np);
            v.apply_preset(((v.preset + dir) % np + np) % np);   // Custom -> Off -> ... -> last preset -> Custom
        } else {
            const VParam& p = kVParams[r.ref];
            const int nv = VideoSettings::clamp(v.*(p.field) + dir * p.step, p.lo, p.hi);
            if (nv == v.*(p.field)) return;
            v.*(p.field) = nv;
            // aspect ratio and integer scaling are window layout choices, not part of a preset
            if (p.field != &VideoSettings::aspect && p.field != &VideoSettings::integer_scale) v.changed_by_hand();
        }
        if (hooks_.video_changed) hooks_.video_changed();
        const int keep = sel_;
        build_rows();
        sel_ = keep;
    } else if (page_ == Page::Cheats && hooks_.cheats) {
        cheat::Cheat& c = hooks_.cheats->cheats()[(size_t)r.ref];
        if (!c.items.empty()) hooks_.cheats->set_param(r.ref, c.param_index + dir);
        else if (dir != 0) hooks_.cheats->set_enabled(r.ref, dir > 0);
        build_rows();
    }
}

void Menu::activate() {
    if (sel_ < 0 || sel_ >= (int)rows_.size()) return;
    const Row& r = rows_[(size_t)sel_];
    if (page_ == Page::Dip) {
        if (r.kind == 2 && r.ref == -2) {
            if (hooks_.set_dsw) hooks_.set_dsw(kDipDefault);
            if (hooks_.settings_changed) hooks_.settings_changed();
            const int s = sel_;
            build_rows();
            sel_ = std::min(s, (int)rows_.size() - 1);
        } else if (r.kind == 2 && r.ref == -3) {
            if (hooks_.reset_game) hooks_.reset_game();
            close();
        } else if (r.kind == 1) {
            change(1);
        }
    } else if (page_ == Page::Video && hooks_.video) {
        if (r.kind == 2 && r.ref == -5) {
            const int aspect = hooks_.video->aspect, integer = hooks_.video->integer_scale;
            *hooks_.video = VideoSettings();
            hooks_.video->aspect = aspect;   // window layout choices are kept
            hooks_.video->integer_scale = integer;
            if (hooks_.video_changed) hooks_.video_changed();
            const int s = sel_;
            build_rows();
            sel_ = std::min(s, (int)rows_.size() - 1);
        } else if (r.kind == 1) {
            change(1);
        }
    } else if (page_ == Page::Cheats && hooks_.cheats) {
        if (r.kind == 2 && r.ref == -4) {
            hooks_.cheats->disable_all();
            build_rows();
        } else if (r.kind == 1) {
            hooks_.cheats->set_enabled(r.ref, !hooks_.cheats->cheats()[(size_t)r.ref].enabled);
            build_rows();
        }
    }
}

void Menu::key(Key k) {
    if (!active()) return;
    switch (k) {
    case Key::Up: sel_ = move(sel_, -1); break;
    case Key::Down: sel_ = move(sel_, 1); break;
    case Key::Left: change(-1); break;
    case Key::Right: change(1); break;
    case Key::Enter: activate(); break;
    case Key::Back: close(); break;
    }
}

void Menu::render(uint32_t* argb) {
    std::fill(argb, argb + kWidth * kHeight, 0u);
    if (!active()) return;
    Canvas cv{argb, kWidth, kHeight};
    const int px0 = 12, py0 = 12, pw = kWidth - 24, ph = kHeight - 24;
    cv.rect(px0, py0, pw, ph, kBorder);
    cv.rect(px0 + 2, py0 + 2, pw - 4, ph - 4, kPanel);

    const bool dip = page_ == Page::Dip;
    cv.text(px0 + 12, py0 + 10, dip ? "DIP SWITCHES (SW1 / SW2)" : page_ == Page::Video ? "VIDEO / CRT SHADER" : "CHEATS", kTitle);
    if (dip) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "DSW=%04X", hooks_.get_dsw ? hooks_.get_dsw() : 0);
        cv.text(px0 + pw - 12 - 8 * 8, py0 + 10, buf, kDimText);
    }
    cv.rect(px0 + 8, py0 + 22, pw - 16, 1, kBorder);

    const int row_h = 11, list_y = py0 + 30, footer_h = 66;
    const int visible = (ph - 30 - footer_h) / row_h;
    if (sel_ < top_) top_ = sel_;
    if (sel_ >= top_ + visible) top_ = sel_ - visible + 1;
    top_ = std::max(0, std::min(top_, std::max(0, (int)rows_.size() - visible)));

    const int right_x = px0 + 12 + 37 * 8;
    for (int i = top_; i < (int)rows_.size() && i < top_ + visible; i++) {
        const Row& r = rows_[(size_t)i];
        const int y = list_y + (i - top_) * row_h;
        const bool sel = i == sel_ && (r.kind == 1 || r.kind == 2);
        if (sel) cv.rect(px0 + 6, y - 2, pw - 12, row_h, kSelBg);
        uint32_t col = r.dim ? kDimText : kText;
        if (r.kind == 2) col = kHelp;
        if (!r.right.empty() || r.kind == 1) {
            if (page_ == Page::Cheats && r.left.size() > 1 && r.left[1] == 'x') col = sel ? 0xFFFFFFFF : kOn;
        }
        cv.text(px0 + 12, y, clip(r.left, 37), col);
        if (!r.right.empty()) {
            const std::string v = sel ? "< " + r.right + " >" : r.right;
            cv.text(right_x, y, clip(v, 23), sel ? 0xFFFFFFFF : (page_ == Page::Dip ? kOn : page_ == Page::Video ? kOn : kText));
        }
    }
    if (top_ > 0) cv.text(px0 + pw - 24, list_y - 10, "^", kHelp);
    if (top_ + visible < (int)rows_.size()) cv.text(px0 + pw - 24, list_y + visible * row_h - 4, "v", kHelp);

    // footer: description of the selected entry + key help
    const int fy = py0 + ph - footer_h;
    cv.rect(px0 + 8, fy, pw - 16, 1, kBorder);
    std::vector<std::string> info;
    // `ref` is an index into a table for ordinary rows, but a negative or special value for actions and notes:
    // always check before using it as an index.
    const Row* sr = (sel_ >= 0 && sel_ < (int)rows_.size()) ? &rows_[(size_t)sel_] : nullptr;
    const cheat::Cheat* sel_cheat = nullptr;
    if (page_ == Page::Cheats && sr && hooks_.cheats && sr->ref >= 0 && sr->ref < (int)hooks_.cheats->cheats().size())
        sel_cheat = &hooks_.cheats->cheats()[(size_t)sr->ref];
    const VParam* sel_vparam = (page_ == Page::Video && sr && sr->kind == 1 && sr->ref >= 0 && sr->ref < kVParamCount) ? &kVParams[sr->ref] : nullptr;

    if (dip) {
        info = wrap("Most DIP switch changes only take effect after a game reset (F3, or the entry above). "
                    "Settings are saved to bloodbro.ini.", 58);
    } else if (page_ == Page::Video) {
        info = wrap(hooks_.shader_available
                        ? "Changes apply at once and are saved to bloodbro.ini. Integer scaling and 4:3 look best with scanlines."
                        : "Neither Vulkan nor OpenGL is in use: scanlines, mask and the other effects are disabled.",
                    58);
        if (!hooks_.bezel_available && sel_vparam && std::strcmp(sel_vparam->name, "Bezel (monitor frame)") == 0)
            info = wrap("No bezel image found. Put a PNG with a transparent screen area at bezel/bloodbro.png next to the program.", 58);
    } else if (sel_cheat) {
        if (!sel_cheat->error.empty()) info = wrap("Cannot use this cheat: " + sel_cheat->error, 58);
        else info = wrap(sel_cheat->comment, 58);
    }
    for (size_t i = 0; i < info.size() && i < 3; i++) cv.text(px0 + 12, fy + 6 + (int)i * 10, info[i], kWarn);
    cv.text(px0 + 12, py0 + ph - 30, "Up/Down: select    Left/Right: change value", kHelp);
    cv.text(px0 + 12, py0 + ph - 20, dip ? "Enter: action    F1/Esc: close" : page_ == Page::Video ? "Enter: next value    F4/Esc: close" : "Enter: switch on/off    F2/Esc: close", kHelp);
}
