// Launcher window: a Dear ImGui dashboard (SDL3 + OpenGL 2) that edits bloodbro.ini (the same file the game
// reads and writes) and starts the game. It is part of bloodbro.exe: started without arguments the program
// shows this window, and PLAY runs the program again as "bloodbro.exe --play". The launcher waits in the
// background and reloads the settings when the game ends (the F1/F2/F4 menus may have changed them).
#ifdef _WIN32

#include "launcher.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "cheats.h"
#include "dips.h"
#include "imgui.h"
#include "imgui_impl_opengl2.h"
#include "imgui_impl_sdl3.h"
#include "ini.h"
#include "romset.h"
#include "video_settings.h"

#include "cheats_builtin.inc"  // kBuiltinCheatChunks

namespace {

// ---- colours ----
const ImVec4 kAccent(0.80f, 0.12f, 0.14f, 1.0f);
const ImVec4 kAccentHover(0.92f, 0.20f, 0.20f, 1.0f);
const ImVec4 kAccentActive(0.65f, 0.08f, 0.10f, 1.0f);
const ImVec4 kGood(0.35f, 0.80f, 0.40f, 1.0f);
const ImVec4 kWarn(0.95f, 0.75f, 0.25f, 1.0f);
const ImVec4 kBad(0.95f, 0.30f, 0.30f, 1.0f);
const ImVec4 kDim(0.60f, 0.60f, 0.65f, 1.0f);

const char* const kFilterLabels[3] = {"Sharp bilinear", "Linear", "Nearest"};
const char* const kAspectLabels[3] = {"4:3 (arcade monitor)", "Square pixels", "Stretch to window"};

enum class Page { Game, Video, Controls, Rules, Cheats };

// ---- state ----
SDL_Window* g_window = nullptr;
ImFont* g_font_bold = nullptr;
std::string g_exe;       // full path of bloodbro.exe
std::string g_dir;       // its folder, with trailing backslash
std::string g_ini_path;
Ini g_ini;
VideoSettings g_vs;
uint16_t g_dsw = kDipDefault;
int g_scale = 3;
bool g_fullscreen = false, g_vsync = true, g_sound = true, g_close_after = false, g_skip = false;
char g_rom[MAX_PATH * 3] = "";   // UTF-8 (shown and edited); the ini stores the ANSI code page like the game expects
Page g_page = Page::Game;
HANDLE g_game = nullptr;
std::string g_bezel_file;        // found bezel picture (empty: none)

// cheats: the list comes from the cheat file the game uses (or the built-in list); only their switches are stored
cheat::Engine g_cheats;
bool g_cheats_ok = false;
std::string g_cheats_error;

// result of checking the ROM set
struct RomCheck {
    std::string path;      // the ANSI path that was checked
    enum { None, Ok, Warn, Missing, Bad } state = None;
    std::string title, detail;
} g_check;

// a message shown in a modal popup
std::string g_popup_title, g_popup_text;
bool g_popup_ask = false;      // with "Start anyway" / "Cancel"

// ---- helpers ----

std::string convert(const std::string& s, UINT from, UINT to) {
    if (s.empty()) return s;
    const int wn = MultiByteToWideChar(from, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(wn, L'\0');
    MultiByteToWideChar(from, 0, s.c_str(), (int)s.size(), &w[0], wn);
    const int n = WideCharToMultiByte(to, 0, w.c_str(), wn, nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(to, 0, w.c_str(), wn, &r[0], n, nullptr, nullptr);
    return r;
}
std::string acp_to_utf8(const std::string& s) { return convert(s, CP_ACP, CP_UTF8); }
std::string utf8_to_acp(const std::string& s) { return convert(s, CP_UTF8, CP_ACP); }

bool file_exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// paths inside the program folder are stored relative to it (the game runs with that folder as working directory)
std::string make_relative(const std::string& p) {
    if (p.size() > g_dir.size() && CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, p.c_str(), (int)g_dir.size(),
                                                  g_dir.c_str(), (int)g_dir.size()) == CSTR_EQUAL)
        return p.substr(g_dir.size());
    return p;
}
std::string absolute(const std::string& p) {
    if (p.size() > 1 && (p[1] == ':' || (p[0] == '\\' && p[1] == '\\'))) return p;
    return g_dir + p;
}

std::string rom_text() {   // the ROM field as ANSI path, without surrounding blanks and quotes
    std::string s = utf8_to_acp(g_rom);
    while (!s.empty() && (s.back() == ' ' || s.back() == '"')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '"')) s.erase(0, 1);
    return s;
}

// the file the game will load: the ROM field, or the places the game searches when it is empty
std::string effective_rom() {
    const std::string r = rom_text();
    if (!r.empty()) return absolute(r);
    for (const char* c : {"roms\\bloodbro.zip", "bloodbro.zip"})
        if (file_exists(g_dir + c)) return g_dir + c;
    return std::string();
}

std::string cheat_key(const std::string& desc) {   // same as the game
    std::string k = desc;
    for (char& ch : k)
        if (ch == '=' || ch == '\n' || ch == '\r' || ch == ';' || ch == '[' || ch == ']') ch = '-';
    return k;
}

void check_rom() {
    const std::string path = effective_rom();
    g_check = RomCheck();
    g_check.path = rom_text();
    if (path.empty()) {
        g_check.state = RomCheck::Missing;
        g_check.title = "No ROM found";
        g_check.detail = "Select bloodbro.zip, or put it next to bloodbro.exe or into a folder named \"roms\".";
        return;
    }
    if (!file_exists(path)) {
        g_check.state = RomCheck::Missing;
        g_check.title = "File not found";
        g_check.detail = acp_to_utf8(path);
        return;
    }
    RomSet set;
    std::string err, warn;
    if (!load_romset({path}, set, err, warn)) {
        g_check.state = RomCheck::Bad;
        g_check.title = "Not usable";
        g_check.detail = err + "\nThe ROM set must be the MAME set \"bloodbro\".";
        return;
    }
    g_check.state = warn.empty() ? RomCheck::Ok : RomCheck::Warn;
    g_check.title = warn.empty() ? "Verified" : "Loaded with warnings";
    g_check.detail = set.set_name + " - " + set.description + "\n" + acp_to_utf8(path);
    if (!warn.empty()) g_check.detail += "\n" + warn;
}

void find_bezel() {
    g_bezel_file.clear();
    const std::string names[] = {g_ini.get("video", "bezel_file", ""), "bezel\\bloodbro.png", "bezels\\bloodbro.png",
                                 "bloodbro_bezel.png", "bezel.png"};
    for (const std::string& n : names)
        if (!n.empty() && file_exists(absolute(n))) { g_bezel_file = n; return; }
}

void load_cheats() {
    g_cheats = cheat::Engine();
    g_cheats_error.clear();
    const std::string file = g_ini.get("game", "cheats", "");
    int n = -1;
    if (!file.empty() && file_exists(absolute(file))) n = g_cheats.load(absolute(file), g_cheats_error);
    if (n < 0) {
        std::string xml;
        for (const char* const* p = kBuiltinCheatChunks; *p; ++p) xml += *p;
        n = g_cheats.load_string(xml, "built-in cheats", g_cheats_error);
    }
    g_cheats_ok = n >= 0;
    if (!g_cheats_ok) return;
    // the switches saved by the game: "1" / "0", with ",N" for the chosen parameter
    for (auto& c : g_cheats.cheats()) {
        if (c.separator) continue;
        const std::string v = g_ini.get("cheats", cheat_key(c.desc), "");
        c.enabled = !v.empty() && v[0] == '1';
        const size_t comma = v.find(',');
        c.param_index = comma == std::string::npos ? 0 : std::atoi(v.c_str() + comma + 1);
        if (c.param_index < 0 || c.param_index >= (int)c.items.size()) c.param_index = 0;
    }
}

void load_settings() {
    g_ini = Ini();
    g_ini.load(g_ini_path);   // a missing file is fine: defaults
    g_vs = VideoSettings();
    g_vs.load(g_ini);
    g_dsw = (uint16_t)std::strtoul(g_ini.get("dip", "dsw", "FFFF").c_str(), nullptr, 16);
    g_scale = VideoSettings::clamp(g_ini.get_int("video", "scale", 3), 1, 8);
    g_fullscreen = g_ini.get_bool("video", "fullscreen", false);
    g_vsync = g_ini.get_bool("video", "vsync", true);
    g_sound = g_ini.get_bool("audio", "sound", true);
    g_close_after = g_ini.get_bool("launcher", "close_after_start", false);
    g_skip = g_ini.get_bool("launcher", "skip", false);
    const std::string rom = acp_to_utf8(g_ini.get("game", "rom", ""));
    std::snprintf(g_rom, sizeof g_rom, "%s", rom.c_str());
    find_bezel();
    load_cheats();
    check_rom();
}

bool save_settings() {
    g_ini.load(g_ini_path);   // pick up anything the game wrote meanwhile
    g_ini.set("game", "rom", rom_text());
    g_ini.set_bool("audio", "sound", g_sound);
    g_vs.save(g_ini);
    g_ini.set_int("video", "scale", g_scale);
    g_ini.set_bool("video", "fullscreen", g_fullscreen);
    g_ini.set_bool("video", "vsync", g_vsync);
    char hex[8];
    std::snprintf(hex, sizeof hex, "%04X", g_dsw);
    g_ini.set("dip", "dsw", hex);
    if (g_cheats_ok) {
        g_ini.clear_section("cheats");
        for (const auto& c : g_cheats.cheats())
            if (!c.separator)
                g_ini.set("cheats", cheat_key(c.desc), std::string(c.enabled ? "1" : "0") + (c.items.empty() ? "" : "," + std::to_string(c.param_index)));
    }
    g_ini.set_bool("launcher", "close_after_start", g_close_after);
    g_ini.set_bool("launcher", "skip", g_skip);
    return g_ini.save(g_ini_path, "; bloodbro.ini - settings of bloodbro.exe. Saved by the game (F1 = DIP switches, F2 = cheats) and by the launcher.");
}

HWND native_window() {
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}

void browse_rom() {
    char file[MAX_PATH] = "";
    const std::string cur = rom_text();
    std::string start_dir = g_dir + "roms";
    if (!cur.empty()) {
        const std::string a = absolute(cur);
        const size_t s = a.find_last_of("\\/");
        if (s != std::string::npos) start_dir = a.substr(0, s);
    }
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = native_window();
    ofn.lpstrFilter = "ROM archive (*.zip)\0*.zip\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = start_dir.c_str();
    ofn.lpstrTitle = "Select the Blood Bros. ROM";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) {
        std::snprintf(g_rom, sizeof g_rom, "%s", acp_to_utf8(make_relative(file)).c_str());
        check_rom();
    }
}

void show_message(const std::string& title, const std::string& text, bool ask = false) {
    g_popup_title = title;
    g_popup_text = text;
    g_popup_ask = ask;
}

// Returns true when the launcher should close (game started with "close the launcher" on).
bool start_game(bool force) {
    if (!force && (g_check.state == RomCheck::Missing || g_check.state == RomCheck::Bad)) {
        show_message("ROM problem", g_check.title + ":\n" + g_check.detail + "\n\nStart anyway?", true);
        return false;
    }
    if (!save_settings()) {
        show_message("Error", "The settings could not be saved:\n" + acp_to_utf8(g_ini_path));
        return false;
    }
    std::string cmd = "\"" + g_exe + "\" --play";
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(g_exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_dir.c_str(), &si, &pi)) {
        show_message("Error", "The game could not be started (error " + std::to_string(GetLastError()) + ").");
        return false;
    }
    CloseHandle(pi.hThread);
    if (g_close_after) {
        CloseHandle(pi.hProcess);
        return true;
    }
    g_game = pi.hProcess;
    SDL_HideWindow(g_window);
    return false;
}

// ---- widgets ----

void heading(const char* text, float size = 26.0f) {
    ImGui::PushFont(g_font_bold, size);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

void help_marker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// a card: a framed child area with a title (close it with end_card)
void begin_card(const char* title) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * ImGui::GetStyle().FontScaleDpi);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.13f, 0.15f, 1.0f));
    ImGui::BeginChild(title, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(g_font_bold, 18.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.97f, 1.0f), "%s", title);
    ImGui::PopFont();
    ImGui::Spacing();
}
void end_card() {
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::Spacing();
}

// label in the left column, widget in the right one
void row_label(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 11.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool combo(const char* label, int* v, const char* const* items, int count) {
    row_label(label);
    ImGui::PushID(label);
    const bool r = ImGui::Combo("##c", v, items, count);
    ImGui::PopID();
    return r;
}

bool slider(const char* label, int* v, int lo, int hi) {
    row_label(label);
    ImGui::PushID(label);
    const bool r = ImGui::SliderInt("##s", v, lo, hi, "%d %%");
    ImGui::PopID();
    return r;
}

void status_dot(const ImVec4& col) {
    const float r = ImGui::GetFontSize() * 0.3f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f), r, ImGui::GetColorU32(col));
    ImGui::Dummy(ImVec2(r * 2.0f, ImGui::GetTextLineHeight()));
    ImGui::SameLine();
}

ImVec4 check_color() {
    switch (g_check.state) {
    case RomCheck::Ok: return kGood;
    case RomCheck::Warn: return kWarn;
    default: return kBad;
    }
}

// ---- pages ----

void page_game() {
    heading("Game");
    ImGui::TextColored(kDim, "The ROM set and sound.");
    ImGui::Spacing();

    begin_card("ROM set");
    ImGui::TextWrapped("Only the MAME set \"bloodbro\" works: the recompiled program is a translation of exactly this program.");
    ImGui::Spacing();
    const float browse_w = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    row_label("ROM (ZIP)");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browse_w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##rom", "empty: roms\\bloodbro.zip or bloodbro.zip next to the game", g_rom, sizeof g_rom);
    if (ImGui::IsItemDeactivatedAfterEdit()) check_rom();
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) browse_rom();
    ImGui::Spacing();
    status_dot(check_color());
    ImGui::PushFont(g_font_bold, 0.0f);
    ImGui::TextColored(check_color(), "%s", g_check.title.c_str());
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", g_check.detail.c_str());
    ImGui::PopStyleColor();
    if (ImGui::Button("Check again")) check_rom();
    end_card();

    begin_card("Sound");
    ImGui::Checkbox("Sound on", &g_sound);
    end_card();
}

void page_video() {
    heading("Video");
    ImGui::TextColored(kDim, "Window, picture and CRT effects. The F4 menu in the game has the same settings.");
    ImGui::Spacing();

    begin_card("Window");
    static const char* const kScales[8] = {"1x (224 lines)", "2x (448 lines)", "3x (672 lines)", "4x (896 lines)",
                                           "5x (1120 lines)", "6x (1344 lines)", "7x (1568 lines)", "8x (1792 lines)"};
    int scale = g_scale - 1;
    if (combo("Window size", &scale, kScales, 8)) g_scale = scale + 1;
    combo("Aspect ratio", &g_vs.aspect, kAspectLabels, 3);
    ImGui::Checkbox("Fullscreen", &g_fullscreen);
    ImGui::SameLine(ImGui::GetFontSize() * 11.0f);
    bool integer = g_vs.integer_scale != 0;
    if (ImGui::Checkbox("Integer scaling only", &integer)) g_vs.integer_scale = integer ? 1 : 0;
    help_marker("Scales the picture by whole numbers only. Looks best with scanlines.");
    ImGui::Checkbox("VSync", &g_vsync);
    combo("Renderer", &g_vs.renderer, VideoSettings::kRendererLabels, 4);
    help_marker("Automatic tries Vulkan first, then OpenGL. If the chosen one does not work, the game uses the other one, "
                "and as a last resort the basic SDL renderer. The CRT effects need Vulkan or OpenGL.");
    end_card();

    begin_card("Picture");
    int n;
    const VideoSettings::Preset* p = VideoSettings::presets(n);
    std::vector<const char*> names;
    for (int i = 0; i < n; i++) names.push_back(i == 0 ? "Custom (own settings)" : p[i].label);
    int preset = g_vs.preset;
    if (combo("Preset", &preset, names.data(), n)) g_vs.apply_preset(preset);
    ImGui::TextColored(kDim, "Changing a value below switches the preset to Custom.");
    ImGui::Spacing();
    bool by_hand = false;
    by_hand |= combo("Pixel filter", &g_vs.filter, kFilterLabels, 3);
    bool bezel = g_vs.bezel != 0;
    row_label("Monitor frame");
    if (ImGui::Checkbox("Show bezel##bezel", &bezel)) { g_vs.bezel = bezel ? 1 : 0; by_hand = true; }
    ImGui::SameLine();
    if (g_bezel_file.empty()) ImGui::TextColored(kDim, "(no bezel picture found)");
    else ImGui::TextColored(kDim, "(%s)", acp_to_utf8(g_bezel_file).c_str());
    help_marker("A PNG with a transparent screen window: bezel\\bloodbro.png next to bloodbro.exe, "
                "or bezel_file= under [video] in bloodbro.ini.");
    by_hand |= slider("Scanlines", &g_vs.scanlines, 0, 100);
    by_hand |= slider("Shadow mask", &g_vs.mask, 0, 100);
    by_hand |= slider("Screen curvature", &g_vs.curvature, 0, 100);
    by_hand |= slider("Glow", &g_vs.glow, 0, 100);
    by_hand |= slider("Vignette", &g_vs.vignette, 0, 100);
    by_hand |= slider("Phosphor afterglow", &g_vs.persistence, 0, 100);
    by_hand |= slider("Brightness", &g_vs.brightness, 50, 150);
    by_hand |= slider("Saturation", &g_vs.saturation, 0, 150);
    if (by_hand) g_vs.changed_by_hand();
    end_card();
}

void key_table(const char* id, const char* const (*rows)[3], int count, const char* h1, const char* h2) {
    if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn(h1, ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(h2, ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();
    for (int i = 0; i < count; i++) {
        ImGui::TableNextRow();
        for (int c = 0; c < 3; c++) {
            ImGui::TableSetColumnIndex(c);
            if (c == 0) ImGui::TextColored(kDim, "%s", rows[i][c]);
            else ImGui::TextUnformatted(rows[i][c]);
        }
    }
    ImGui::EndTable();
}

void page_controls() {
    heading("Controls");
    ImGui::TextColored(kDim, "Keyboard and gamepads. Gamepads are found automatically, also while the game runs.");
    ImGui::Spacing();

    begin_card("Keyboard");
    static const char* const kKeys[][3] = {
        {"Move", "Arrow keys", "I / J / K / L"}, {"Fire", "Z", "U"}, {"Roll", "X", "O"}, {"Dynamite", "C", "P"},
        {"Coin", "5", "6"}, {"Start", "1", "2"},
    };
    key_table("keys", kKeys, (int)(sizeof kKeys / sizeof kKeys[0]), "Player 1", "Player 2");
    end_card();

    begin_card("Gamepad");
    static const char* const kPad[][3] = {
        {"Move", "D-pad / left stick", "Hat / axis 0+1"}, {"Fire", "A", "Button 0"}, {"Roll", "B", "Button 1"},
        {"Dynamite", "X / Y", "Button 2 / 3"}, {"Coin", "Back", "Button 6 / 8"}, {"Start", "Start", "Button 7 / 9"},
    };
    key_table("pad", kPad, (int)(sizeof kPad / sizeof kPad[0]), "Known pad", "Other pads");
    ImGui::Spacing();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (count <= 0) ImGui::TextColored(kDim, "No gamepad connected.");
    for (int i = 0; i < count && i < 4; i++) {
        status_dot(i < 2 ? kGood : kDim);
        const bool mapped = SDL_IsGamepad(ids[i]);
        const char* name = mapped ? SDL_GetGamepadNameForID(ids[i]) : SDL_GetJoystickNameForID(ids[i]);
        ImGui::Text("%s: %s%s", i < 2 ? (i == 0 ? "Player 1" : "Player 2") : "Not used", name ? name : "Gamepad",
                    mapped ? "" : "  (fallback mapping)");
    }
    SDL_free(ids);
    end_card();

    begin_card("Hotkeys in the game");
    static const char* const kHot[][3] = {
        {"DIP switches menu", "F1", ""}, {"Cheats menu", "F2", ""}, {"Reset", "F3", ""}, {"Video menu", "F4", ""},
        {"Next pixel filter", "F10", ""}, {"Fullscreen", "F11", ""}, {"Service", "9", ""}, {"Quit", "Esc", ""},
    };
    key_table("hot", kHot, (int)(sizeof kHot / sizeof kHot[0]), "Key", "");
    end_card();
}

void page_rules() {
    heading("Game Rules");
    ImGui::TextColored(kDim, "The DIP switches of the arcade board. Some only take effect after a reset (F3 in the game).");
    ImGui::Spacing();

    begin_card("DIP switches");
    for (int k = 0; k < kDipCount; k++) {
        const DipDef& d = kDips[k];
        if (std::string(d.name) == "Unused") continue;
        if (d.cond_mask && (g_dsw & d.cond_mask) != d.cond_value) continue;   // Coinage only in coin mode 1, Coin A/B only in mode 2
        int sel = -1;
        for (int o = 0; o < d.count; o++)
            if ((g_dsw & d.mask) == d.opts[o].value) sel = o;
        std::vector<const char*> labels;
        for (int o = 0; o < d.count; o++) labels.push_back(d.opts[o].label);
        row_label(d.name);
        ImGui::PushID(k);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 5.0f);
        if (ImGui::Combo("##dip", &sel, labels.data(), d.count) && sel >= 0)
            g_dsw = (uint16_t)((g_dsw & ~d.mask) | d.opts[sel].value);
        ImGui::SameLine();
        ImGui::TextColored(kDim, "%s", d.loc);
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (ImGui::Button("Restore defaults")) g_dsw = kDipDefault;
    end_card();
}

void page_cheats() {
    heading("Cheats");
    ImGui::TextColored(kDim, "From the MAME cheat project. The F2 menu in the game has the same list.");
    ImGui::Spacing();

    begin_card("Cheat list");
    if (!g_cheats_ok) {
        ImGui::TextColored(kBad, "No cheats available: %s", g_cheats_error.c_str());
        end_card();
        return;
    }
    auto& list = g_cheats.cheats();
    for (size_t i = 0; i < list.size(); i++) {
        cheat::Cheat& c = list[i];
        if (c.separator) { ImGui::Separator(); continue; }
        ImGui::PushID((int)i);
        ImGui::BeginDisabled(!c.error.empty());
        ImGui::Checkbox(c.desc.c_str(), &c.enabled);
        ImGui::EndDisabled();
        if (!c.error.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kBad, "[!]");
            if (ImGui::BeginItemTooltip()) { ImGui::TextUnformatted(c.error.c_str()); ImGui::EndTooltip(); }
        } else if (!c.comment.empty() && ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(c.comment.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        if (!c.items.empty()) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##param", c.items[c.param_index].label.c_str())) {
                for (size_t o = 0; o < c.items.size(); o++)
                    if (ImGui::Selectable(c.items[o].label.c_str(), (int)o == c.param_index)) c.param_index = (int)o;
                ImGui::EndCombo();
            }
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (ImGui::Button("Switch all off"))
        for (auto& c : list) c.enabled = false;
    end_card();
}

// ---- layout ----

void apply_theme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 0.0f;
    s.FrameRounding = 6.0f;
    s.GrabRounding = 6.0f;
    s.PopupRounding = 6.0f;
    s.ScrollbarRounding = 6.0f;
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.WindowPadding = ImVec2(16, 16);
    s.ScrollbarSize = 12.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 1.0f;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.09f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.22f, 0.22f, 0.25f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.18f, 0.21f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.24f, 0.28f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.28f, 0.33f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.22f, 0.22f, 0.26f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.30f, 0.35f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.35f, 0.35f, 0.40f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.80f, 0.12f, 0.14f, 0.55f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.80f, 0.12f, 0.14f, 0.35f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.80f, 0.12f, 0.14f, 0.75f);
    c[ImGuiCol_CheckMark] = kAccentHover;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccentHover;
    c[ImGuiCol_Separator] = c[ImGuiCol_Border];
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.18f, 0.18f, 0.21f, 1.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_NavCursor] = kAccentHover;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
}

void nav_button(const char* label, Page page) {
    const float h = ImGui::GetFontSize() * 2.2f;
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushFont(g_font_bold, 17.0f);
    if (ImGui::Selectable(label, g_page == page, 0, ImVec2(0, h))) g_page = page;
    ImGui::PopFont();
    ImGui::PopStyleVar();
}

// Draws one frame of the launcher. Returns true when the launcher should close because the game started.
bool draw_ui(bool& quit) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    const float fs = ImGui::GetFontSize();
    const float footer_h = fs * 4.2f;
    const float sidebar_w = fs * 15.0f;
    bool close = false;

    // ---- sidebar ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.11f, 0.12f, 1.0f));
    ImGui::BeginChild("sidebar", ImVec2(sidebar_w, -footer_h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        // title banner
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float bw = ImGui::GetContentRegionAvail().x, bh = fs * 6.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilledMultiColor(p, ImVec2(p.x + bw, p.y + bh), IM_COL32(150, 18, 22, 255), IM_COL32(70, 8, 12, 255),
                                    IM_COL32(25, 4, 6, 255), IM_COL32(90, 10, 14, 255));
        dl->AddRect(p, ImVec2(p.x + bw, p.y + bh), IM_COL32(200, 40, 40, 120), 0.0f, 0, 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + fs * 0.8f, p.y + fs * 0.9f));
        ImGui::PushFont(g_font_bold, 32.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.93f, 0.85f, 1.0f), "BLOOD");
        ImGui::SetCursorScreenPos(ImVec2(p.x + fs * 0.8f, ImGui::GetCursorScreenPos().y - fs * 0.6f));
        ImGui::TextColored(ImVec4(1.0f, 0.93f, 0.85f, 1.0f), "BROS.");
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + bh + fs * 0.5f));
        ImGui::TextColored(kDim, "TAD Corporation, 1990");
        ImGui::TextColored(kDim, "Static recompilation");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        nav_button("Game", Page::Game);
        nav_button("Video", Page::Video);
        nav_button("Controls", Page::Controls);
        nav_button("Game Rules", Page::Rules);
        nav_button("Cheats", Page::Cheats);

        // ROM status at the bottom
        const float status_h = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
        const float y = ImGui::GetWindowHeight() - status_h - ImGui::GetStyle().WindowPadding.y;
        if (ImGui::GetCursorPosY() < y) ImGui::SetCursorPosY(y);
        ImGui::TextColored(kDim, "ROM");
        status_dot(check_color());
        ImGui::TextColored(check_color(), "%s", g_check.title.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---- page ----
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 1.4f, fs * 1.2f));
    ImGui::BeginChild("page", ImVec2(0, -footer_h), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    switch (g_page) {
    case Page::Game: page_game(); break;
    case Page::Video: page_video(); break;
    case Page::Controls: page_controls(); break;
    case Page::Rules: page_rules(); break;
    case Page::Cheats: page_cheats(); break;
    }
    ImGui::EndChild();

    // ---- footer ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.11f, 0.12f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 1.2f, 0));
    ImGui::BeginChild("footer", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    {
        ImGui::SetCursorPosY((footer_h - ImGui::GetFrameHeight()) * 0.5f);
        ImGui::Checkbox("Close the launcher when the game starts", &g_close_after);
        ImGui::SameLine(0, fs * 1.5f);
        bool skip = g_skip;
        if (ImGui::Checkbox("Skip the launcher next time", &skip)) {
            if (skip) ImGui::OpenPopup("Skip the launcher?");
            else g_skip = false;
        }
        if (ImGui::BeginPopupModal("Skip the launcher?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("bloodbro.exe will start the game directly from now on.");
            ImGui::TextUnformatted("Hold Shift while starting bloodbro.exe to open the launcher again.");
            ImGui::Spacing();
            if (ImGui::Button("Skip the launcher", ImVec2(fs * 10, 0))) { g_skip = true; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(fs * 6, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        const float btn_h = fs * 2.6f, play_w = fs * 9.0f, quit_w = fs * 6.0f;
        const float x = ImGui::GetWindowWidth() - play_w - quit_w - ImGui::GetStyle().ItemSpacing.x - fs * 1.2f;
        ImGui::SetCursorPos(ImVec2(x, (footer_h - btn_h) * 0.5f));
        ImGui::PushFont(g_font_bold, 18.0f);
        if (ImGui::Button("QUIT", ImVec2(quit_w, btn_h))) quit = true;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
        if (ImGui::Button("PLAY", ImVec2(play_w, btn_h))) close = start_game(false);
        ImGui::PopStyleColor(3);
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---- messages ----
    if (!g_popup_title.empty() && !ImGui::IsPopupOpen("##message")) ImGui::OpenPopup("##message");
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##message", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        heading(g_popup_title.c_str(), 20.0f);
        ImGui::Spacing();
        ImGui::PushTextWrapPos(fs * 32.0f);
        ImGui::TextUnformatted(g_popup_text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (g_popup_ask) {
            if (ImGui::Button("Start anyway", ImVec2(fs * 8, 0))) {
                g_popup_title.clear();
                ImGui::CloseCurrentPopup();
                close = start_game(true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(fs * 6, 0))) { g_popup_title.clear(); ImGui::CloseCurrentPopup(); }
        } else if (ImGui::Button("OK", ImVec2(fs * 6, 0))) {
            g_popup_title.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
    return close;
}

void load_fonts(float base_size) {
    ImGuiIO& io = ImGui::GetIO();
    char windir[MAX_PATH];
    const UINT n = GetWindowsDirectoryA(windir, MAX_PATH);
    const std::string fonts = std::string(windir, n) + "\\Fonts\\";
    ImFont* regular = nullptr;
    if (file_exists(fonts + "segoeui.ttf")) regular = io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(), base_size);
    if (!regular) regular = io.Fonts->AddFontDefault();
    if (file_exists(fonts + "segoeuib.ttf")) g_font_bold = io.Fonts->AddFontFromFileTTF((fonts + "segoeuib.ttf").c_str(), base_size);
    if (!g_font_bold) g_font_bold = regular;
    io.FontDefault = regular;
    ImGui::GetStyle().FontSizeBase = base_size;
}

}  // namespace

bool launcher_skipped() {
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path, n);
    const size_t slash = dir.find_last_of("\\/");
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
    Ini ini;
    if (!ini.load(dir + "bloodbro.ini") || !ini.get_bool("launcher", "skip", false)) return false;
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0;   // Shift held: show the launcher anyway
}

int run_launcher() {
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    g_exe.assign(path, n);
    const size_t slash = g_exe.find_last_of("\\/");
    g_dir = slash == std::string::npos ? std::string() : g_exe.substr(0, slash + 1);
    g_ini_path = g_dir + "bloodbro.ini";

    // only the launcher process is DPI aware (sharp text); the game keeps the usual window scaling
    SetProcessDPIAware();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        MessageBoxA(nullptr, (std::string("SDL could not start: ") + SDL_GetError()).c_str(), "Blood Bros.", MB_ICONERROR);
        return 1;
    }
    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    float content_scale = display ? SDL_GetDisplayContentScale(display) : 1.0f;
    if (content_scale <= 0.0f) content_scale = 1.0f;
    const float scale = std::max(1.0f, content_scale);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    int w = (int)(1000 * scale), h = (int)(680 * scale);
    SDL_Rect usable;
    if (display && SDL_GetDisplayUsableBounds(display, &usable)) {
        w = std::min(w, usable.w - 40);
        h = std::min(h, usable.h - 60);
    }
    g_window = SDL_CreateWindow("Blood Bros. Launcher", w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (g_window) SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_GLContext gl = g_window ? SDL_GL_CreateContext(g_window) : nullptr;
    if (!gl) {
        MessageBoxA(nullptr, (std::string("The launcher window could not be created: ") + SDL_GetError()).c_str(), "Blood Bros.", MB_ICONERROR);
        if (g_window) SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(g_window, (int)(760 * scale), (int)(520 * scale));
    SDL_GL_MakeCurrent(g_window, gl);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    apply_theme(scale);
    load_fonts(16.0f);
    ImGui_ImplSDL3_InitForOpenGL(g_window, gl);
    ImGui_ImplOpenGL2_Init();

    load_settings();

    bool quit = false, close = false;
    while (!quit && !close) {
        if (g_game) {   // the game runs: wait for it, then show the launcher again with the saved settings
            SDL_PumpEvents();
            SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
            if (WaitForSingleObject(g_game, 100) != WAIT_OBJECT_0) continue;
            CloseHandle(g_game);
            g_game = nullptr;
            load_settings();   // the game saves its own changes (F1/F2/F4 menus) on exit
            SDL_ShowWindow(g_window);
            SDL_RaiseWindow(g_window);
            continue;
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) quit = true;
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(g_window)) quit = true;
        }
        if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(50);
            continue;
        }
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        close = draw_ui(quit);
        ImGui::Render();
        int dw, dh;
        SDL_GetWindowSizeInPixels(g_window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(g_window);
    }
    if (quit) save_settings();   // keep what was changed, also without playing

    if (g_game) CloseHandle(g_game);
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}

#endif  // _WIN32
