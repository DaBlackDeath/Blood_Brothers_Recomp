// Launcher window: a small Win32 dialog that edits bloodbro.ini (the same file the game reads and writes)
// and starts the game. It is part of bloodbro.exe: started without arguments the program shows this
// window, and "Play" runs the program again as "bloodbro.exe --play". The launcher waits in the
// background and reloads the settings when the game ends (the F1/F4 menus may have changed them).
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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "dips.h"
#include "ini.h"
#include "video_settings.h"

namespace {

enum {
    ID_ROM = 100, ID_ROM_BROWSE, ID_SOUND,
    ID_PRESET, ID_FILTER, ID_ASPECT, ID_SCALE, ID_FULLSCREEN, ID_INTEGER, ID_BEZEL, ID_VSYNC,
    ID_CLOSE_AFTER, ID_DIP_FIRST = 200,
    WM_GAME_EXITED = WM_APP + 1,
};

const char* const kFilterLabels[3] = {"Sharp bilinear", "Linear", "Nearest"};
const char* const kAspectLabels[3] = {"4:3 (arcade monitor)", "Square pixels", "Stretch to window"};
// DIP switches shown in the launcher (the rest stays reachable with F1 in the game)
const char* const kDipNames[] = {"Lives", "Difficulty", "Bonus Life", "Allow Continue", "Coinage", "Demo Sounds"};
const char* const kDipLabels[] = {"Lives", "Difficulty", "Bonus life", "Continue", "Coinage", "Demo sounds"};
constexpr int kDipShown = (int)(sizeof(kDipNames) / sizeof(kDipNames[0]));

HWND g_wnd;
HFONT g_font;
int g_dpi = 96;
std::string g_exe;       // full path of bloodbro.exe
std::string g_dir;       // its folder, with trailing backslash
std::string g_ini_path;
Ini g_ini;
VideoSettings g_vs;
uint16_t g_dsw = kDipDefault;
int g_dip_index[kDipShown];  // index into kDips
HANDLE g_game = nullptr;
HWND g_focus = nullptr;      // control with the keyboard focus while the window is inactive

int S(int v) { return MulDiv(v, g_dpi, 96); }
HWND item(int id) { return GetDlgItem(g_wnd, id); }

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

// ---- building the window ----

HWND add(const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id = 0, DWORD ex = 0) {
    HWND h_ = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                              (HMENU)(INT_PTR)id, GetModuleHandleA(nullptr), nullptr);
    SendMessageA(h_, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h_;
}
void label(const char* text, int x, int y, int w) { add("STATIC", text, SS_LEFT, x, y + 3, w, 18); }
HWND combo(int id, int x, int y, int w) {
    return add("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, 300, id);
}
HWND check(int id, const char* text, int x, int y, int w) {
    return add("BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 22, id);
}
void group(const char* text, int x, int y, int w, int h) { add("BUTTON", text, BS_GROUPBOX, x, y, w, h); }
void combo_add(int id, const char* text) { SendMessageA(item(id), CB_ADDSTRING, 0, (LPARAM)text); }
int combo_sel(int id) { return (int)SendMessageA(item(id), CB_GETCURSEL, 0, 0); }
void combo_set(int id, int i) { SendMessageA(item(id), CB_SETCURSEL, i, 0); }
bool checked(int id) { return SendMessageA(item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
void set_check(int id, bool on) { SendMessageA(item(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

constexpr int kWidth = 520;

int build_controls() {
    const int L = 22, C = 140, CW = kWidth - C - 22;
    int y = 10;

    group("Game", 10, y, kWidth - 20, 90);
    y += 24;
    label("ROM (ZIP):", L, y, 110);
    add("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, C, y, CW - 90, 23, ID_ROM, WS_EX_CLIENTEDGE);
    add("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, C + CW - 84, y, 84, 23, ID_ROM_BROWSE);
    y += 30;
    check(ID_SOUND, "Sound on", C, y, 200);
    y += 40;

    group("Video", 10, y, kWidth - 20, 186);
    y += 24;
    label("Preset:", L, y, 110);
    combo(ID_PRESET, C, y, CW);
    int n;
    const VideoSettings::Preset* p = VideoSettings::presets(n);
    for (int i = 0; i < n; i++) combo_add(ID_PRESET, i == 0 ? "Custom (own settings)" : p[i].label);
    y += 30;
    label("Pixel filter:", L, y, 110);
    combo(ID_FILTER, C, y, CW);
    for (auto* f : kFilterLabels) combo_add(ID_FILTER, f);
    y += 30;
    label("Aspect ratio:", L, y, 110);
    combo(ID_ASPECT, C, y, CW);
    for (auto* a : kAspectLabels) combo_add(ID_ASPECT, a);
    y += 30;
    label("Window size:", L, y, 110);
    combo(ID_SCALE, C, y, CW);
    for (int s = 1; s <= 8; s++) {
        char b[48];
        std::snprintf(b, sizeof b, "%dx  (height %d)", s, 224 * s);
        combo_add(ID_SCALE, b);
    }
    y += 32;
    check(ID_FULLSCREEN, "Fullscreen", C, y, 120);
    check(ID_INTEGER, "Integer scaling only", C + 130, y, 200);
    y += 24;
    check(ID_BEZEL, "Monitor frame (bezel)", C, y, 160);
    check(ID_VSYNC, "VSync", C + 170, y, 120);
    y += 38;

    group("Game rules (DIP switches)", 10, y, kWidth - 20, 120);
    y += 24;
    const int colw = (kWidth - 20) / 2;
    for (int i = 0; i < kDipShown; i++) {
        g_dip_index[i] = -1;
        for (int k = 0; k < kDipCount; k++)
            if (std::string(kDips[k].name) == kDipNames[i]) g_dip_index[i] = k;
        const int cx = 10 + (i % 2) * colw, cy = y + (i / 2) * 30;
        label(kDipLabels[i], cx + 12, cy, 90);
        combo(ID_DIP_FIRST + i, cx + 100, cy, colw - 112);
        if (g_dip_index[i] >= 0) {
            const DipDef& d = kDips[g_dip_index[i]];
            for (int o = 0; o < d.count; o++) combo_add(ID_DIP_FIRST + i, d.opts[o].label);
        }
    }
    y += 3 * 30 + 16;

    add("STATIC", "In game: F1 DIP switches, F2 cheats, F4 video, F11 fullscreen, Esc quit.", SS_LEFT, 12, y, kWidth - 24, 18);
    y += 26;
    check(ID_CLOSE_AFTER, "Close the launcher after starting", 12, y + 4, 240);
    add("BUTTON", "Play", BS_DEFPUSHBUTTON | WS_TABSTOP, kWidth - 200, y, 90, 30, IDOK);
    add("BUTTON", "Quit", BS_PUSHBUTTON | WS_TABSTOP, kWidth - 100, y, 90, 30, IDCANCEL);
    return y + 42;
}

// ---- ini <-> controls ----

void update_dip_enabled() {
    // "Coinage" only exists in coin mode 1 (SW1:1 off); in mode 2 the game uses Coin A / Coin B (F1 menu)
    for (int i = 0; i < kDipShown; i++) {
        if (g_dip_index[i] < 0) { EnableWindow(item(ID_DIP_FIRST + i), FALSE); continue; }
        const DipDef& d = kDips[g_dip_index[i]];
        EnableWindow(item(ID_DIP_FIRST + i), !d.cond_mask || (g_dsw & d.cond_mask) == d.cond_value);
    }
}

void show_look() {
    combo_set(ID_PRESET, g_vs.preset);
    combo_set(ID_FILTER, g_vs.filter);
    set_check(ID_BEZEL, g_vs.bezel != 0);
}

void load_settings() {
    g_ini = Ini();
    g_ini.load(g_ini_path);   // a missing file is fine: defaults
    g_vs = VideoSettings();
    g_vs.load(g_ini);
    g_dsw = (uint16_t)std::strtoul(g_ini.get("dip", "dsw", "FFFF").c_str(), nullptr, 16);

    SetWindowTextA(item(ID_ROM), g_ini.get("game", "rom", "").c_str());
    set_check(ID_SOUND, g_ini.get_bool("audio", "sound", true));

    show_look();
    combo_set(ID_ASPECT, g_vs.aspect);
    combo_set(ID_SCALE, VideoSettings::clamp(g_ini.get_int("video", "scale", 3), 1, 8) - 1);
    set_check(ID_FULLSCREEN, g_ini.get_bool("video", "fullscreen", false));
    set_check(ID_INTEGER, g_vs.integer_scale != 0);
    set_check(ID_VSYNC, g_ini.get_bool("video", "vsync", true));

    for (int i = 0; i < kDipShown; i++) {
        if (g_dip_index[i] < 0) continue;
        const DipDef& d = kDips[g_dip_index[i]];
        int sel = -1;
        for (int o = 0; o < d.count; o++)
            if ((g_dsw & d.mask) == d.opts[o].value) sel = o;
        combo_set(ID_DIP_FIRST + i, sel);
    }
    update_dip_enabled();
    set_check(ID_CLOSE_AFTER, g_ini.get_bool("launcher", "close_after_start", false));
}

std::string window_text(int id) {
    char buf[MAX_PATH * 2];
    GetWindowTextA(item(id), buf, sizeof buf);
    std::string s = buf;
    while (!s.empty() && (s.back() == ' ' || s.back() == '"')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '"')) s.erase(0, 1);
    return s;
}

bool save_settings() {
    g_ini.load(g_ini_path);   // pick up anything the game wrote meanwhile (cheat states ...)
    g_ini.set("game", "rom", window_text(ID_ROM));
    g_ini.set_bool("audio", "sound", checked(ID_SOUND));

    g_vs.aspect = std::max(0, combo_sel(ID_ASPECT));
    g_vs.integer_scale = checked(ID_INTEGER) ? 1 : 0;
    g_vs.save(g_ini);
    g_ini.set_int("video", "scale", combo_sel(ID_SCALE) + 1);
    g_ini.set_bool("video", "fullscreen", checked(ID_FULLSCREEN));
    g_ini.set_bool("video", "vsync", checked(ID_VSYNC));

    char hex[8];
    std::snprintf(hex, sizeof hex, "%04X", g_dsw);
    g_ini.set("dip", "dsw", hex);
    g_ini.set_bool("launcher", "close_after_start", checked(ID_CLOSE_AFTER));
    return g_ini.save(g_ini_path, "; bloodbro.ini - settings of bloodbro.exe. Saved by the game (F1 = DIP switches, F2 = cheats) and by the launcher.");
}

// Filter and bezel belong to the presets: picking a preset sets them ("Custom" brings back the user's own
// values), changing them by hand makes the current values the custom ones.
void on_preset_changed() {
    g_vs.apply_preset(std::max(0, combo_sel(ID_PRESET)));
    show_look();
}
void on_preset_part_changed() {
    g_vs.filter = std::max(0, combo_sel(ID_FILTER));
    g_vs.bezel = checked(ID_BEZEL) ? 1 : 0;
    g_vs.changed_by_hand();
    combo_set(ID_PRESET, 0);
}

void on_dip_changed(int i) {
    const int sel = combo_sel(ID_DIP_FIRST + i);
    if (g_dip_index[i] < 0 || sel < 0) return;
    const DipDef& d = kDips[g_dip_index[i]];
    g_dsw = (uint16_t)((g_dsw & ~d.mask) | d.opts[sel].value);
    update_dip_enabled();
}

void browse_rom() {
    char file[MAX_PATH] = "";
    const std::string cur = window_text(ID_ROM);
    std::string start_dir = g_dir + "roms";
    if (!cur.empty()) {
        const std::string a = absolute(cur);
        const size_t s = a.find_last_of("\\/");
        if (s != std::string::npos) start_dir = a.substr(0, s);
    }
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = "ROM archive (*.zip)\0*.zip\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = start_dir.c_str();
    ofn.lpstrTitle = "Select the Blood Bros. ROM";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) SetWindowTextA(item(ID_ROM), make_relative(file).c_str());
}

DWORD WINAPI wait_for_game(LPVOID h) {
    WaitForSingleObject((HANDLE)h, INFINITE);
    PostMessageA(g_wnd, WM_GAME_EXITED, 0, 0);
    return 0;
}

void start_game() {
    const std::string rom = window_text(ID_ROM);
    if (rom.empty()) {
        if (!file_exists(g_dir + "roms\\bloodbro.zip") && !file_exists(g_dir + "bloodbro.zip") &&
            MessageBoxA(g_wnd, "No ROM selected, and neither roms\\bloodbro.zip nor bloodbro.zip is next to the game.\n\nStart anyway?",
                        "Blood Bros.", MB_ICONWARNING | MB_YESNO) != IDYES)
            return;
    } else if (!file_exists(absolute(rom))) {
        MessageBoxA(g_wnd, ("The ROM file does not exist:\n" + absolute(rom)).c_str(), "Blood Bros.", MB_ICONERROR);
        return;
    }
    if (!save_settings()) {
        MessageBoxA(g_wnd, ("The settings could not be saved:\n" + g_ini_path).c_str(), "Blood Bros.", MB_ICONERROR);
        return;
    }

    std::string cmd = "\"" + g_exe + "\" --play";
    STARTUPINFOA si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(g_exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_dir.c_str(), &si, &pi)) {
        char msg[256];
        std::snprintf(msg, sizeof msg, "The game could not be started (error %lu).", GetLastError());
        MessageBoxA(g_wnd, msg, "Blood Bros.", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    if (checked(ID_CLOSE_AFTER)) {
        CloseHandle(pi.hProcess);
        DestroyWindow(g_wnd);
        return;
    }
    g_game = pi.hProcess;
    ShowWindow(g_wnd, SW_HIDE);
    if (HANDLE t = CreateThread(nullptr, 0, wait_for_game, g_game, 0, nullptr)) CloseHandle(t);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDOK) start_game();
        else if (id == IDCANCEL) DestroyWindow(hwnd);
        else if (id == ID_ROM_BROWSE) browse_rom();
        else if (id == ID_PRESET && code == CBN_SELCHANGE) on_preset_changed();
        else if ((id == ID_FILTER && code == CBN_SELCHANGE) || (id == ID_BEZEL && code == BN_CLICKED)) on_preset_part_changed();
        else if (id >= ID_DIP_FIRST && id < ID_DIP_FIRST + kDipShown && code == CBN_SELCHANGE) on_dip_changed(id - ID_DIP_FIRST);
        return 0;
    }
    case WM_GAME_EXITED:
        if (g_game) { CloseHandle(g_game); g_game = nullptr; }
        load_settings();   // the game saves its own changes (F1/F4 menus) on exit
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        return 0;
    case WM_ACTIVATE:   // keep the keyboard focus on the control that had it (dialog behaviour)
        if (LOWORD(wp) == WA_INACTIVE) {
            HWND f = GetFocus();
            if (f && IsChild(hwnd, f)) g_focus = f;
        } else {
            SetFocus(g_focus ? g_focus : item(IDOK));
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

}  // namespace

int run_launcher() {
    // only the launcher process is DPI aware (sharp text); the game keeps the usual window scaling
    SetProcessDPIAware();
    HINSTANCE inst = GetModuleHandleA(nullptr);
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    g_exe.assign(path, n);
    const size_t slash = g_exe.find_last_of("\\/");
    g_dir = slash == std::string::npos ? std::string() : g_exe.substr(0, slash + 1);
    g_ini_path = g_dir + "bloodbro.ini";

    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    NONCLIENTMETRICSA ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectA(&ncm.lfMessageFont);

    WNDCLASSEXA wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = "BloodBrosLauncher";
    RegisterClassExA(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    g_wnd = CreateWindowExA(WS_EX_CONTROLPARENT, wc.lpszClassName, "Blood Bros.", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            100, 100, nullptr, nullptr, inst, nullptr);
    if (!g_wnd) {
        char msg[96];
        std::snprintf(msg, sizeof msg, "The window could not be created (error %lu).", GetLastError());
        MessageBoxA(nullptr, msg, "Blood Bros.", MB_ICONERROR);
        return 1;
    }
    const int height = build_controls();
    RECT r{0, 0, S(kWidth), S(height)};
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT);
    const int w = r.right - r.left, h = r.bottom - r.top;
    RECT work;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    SetWindowPos(g_wnd, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h, SWP_NOZORDER);

    load_settings();
    g_focus = item(IDOK);
    ShowWindow(g_wnd, SW_SHOWNORMAL);

    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageA(g_wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    if (g_game) CloseHandle(g_game);
    DeleteObject(g_font);
    return 0;
}

#endif  // _WIN32
