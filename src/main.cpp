// Blood Bros. - Windows/SDL3 frontend (Vulkan or OpenGL with the CRT shader, plain SDL renderer as fallback)
//
//   bloodbro [options] [romfile.zip | romdir]
//
// Keys:  arrows = move, Z = fire, X = roll, C = dynamite
//        1/2 = start, 5/6 = coin, 9 = service, F3 = reset, F10 = filter,
//        F11 = fullscreen, Esc = quit.   Player 2: I/J/K/L + U/O/P.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>


#include "bezel.h"
#include "cheats.h"
#include "dips.h"
#include "ini.h"
#include "launcher.h"
#include "machine.h"
#include "menu.h"
#include "miniz.h"
#include "renderer.h"
#include "romset.h"
#include "video_settings.h"

#include "cheats_builtin.inc"  // kBuiltinCheatChunks: the cheat list built into the program (tools/embed_cheats.py)

namespace {

struct Press {
    uint32_t bit;
    uint64_t start;
    uint64_t len;
};
struct Shot {
    uint64_t frame;
    std::string file;
};

struct Options {
    std::vector<std::string> rom_paths;
    int scale = 3;
    bool fullscreen = false;
    bool headless = false;
    bool linear = false;
    bool pixel_aspect = false;  // false: stretch to 4:3 like the arcade monitor
    bool nosound = false;
    bool nothrottle = false;
    bool pad_debug = false;
    std::string dump_prg;
    std::string ini_path;         // default: bloodbro.ini next to the program
    bool use_ini = true;
    std::string cheat_file, ini_cheat_file, ini_rom;
    std::vector<std::pair<std::string, int>> cheats_on;  // --cheat NAME[=PARAM]: cheats to switch on
    std::string menu_shot_page, menu_shot_file;          // --menu-shot dip:file.png | cheats:file.png
    bool dsw_given = false, rom_given = false;
    bool play = false;                  // --play: start the game directly (otherwise, without arguments, the launcher opens)
    std::string renderer;               // --renderer auto|vulkan|opengl|sdl (empty: from bloodbro.ini)
    bool integer_scale = false;         // --integer-scale
    std::string bezel_file;             // --bezel FILE.png
    bool no_bezel = false;
    std::string render_dump_in, render_dump_out;   // --render-dump DUMP:OUT.png
    std::string open_menu_page;         // --open-menu PAGE@FRAME (UI test: open a menu, present once, quit)
    uint64_t open_menu_frame = 0;
    std::string shader_preset;          // --shader off|scanlines|crt|crtstrong
    std::vector<Shot> screen_shots;     // --screen-shot F:FILE: the window contents (with shader) after frame F
    uint32_t watch_write = 0;
    bool autoplay = false;
    uint64_t autoplay_seed = 1;
    uint64_t frames = 0;
    uint16_t dsw = 0xffff;
    std::string wav;
    std::vector<Press> presses;
    std::vector<Shot> shots;
    struct Poke { uint64_t frame; uint32_t addr; uint8_t value; bool every_frame; };
    std::vector<Poke> pokes;
    std::vector<Shot> ram_dumps;
    std::string stage_log;  // prefix: log every "STAGE a-b" banner and dump RAM then  // frame -> file: work RAM at the end of that frame
};

void usage() {
    std::printf(
        "Blood Bros. (statically recompiled)\n\n"
        "usage: bloodbro [options] [rom.zip|romdir]\n"
        "  (without arguments the launcher window opens)\n"
        "  --play           start the game directly with the settings from bloodbro.ini\n"
        "  --rom PATH       ROM zip or directory (repeatable; searched in order); set bloodbro only\n"
        "  --scale N       window scale (default 3)\n"
        "  --fullscreen     start fullscreen\n"
        "  --linear         linear pixel filter instead of sharp bilinear\n"
        "  --pixel-aspect   1:1 pixels (256x224) instead of 4:3\n"
        "  --nosound        disable audio output\n"
        "  --pad-debug      print raw joystick/controller button and axis events\n"
        "  --dsw HEX        DIP switch word (default ffff)\n"
        "test options:\n"
        "  --headless       no window / no audio device\n"
        "  --frames N       quit after N frames\n"
        "  --shot F:FILE    save frame F as PNG (repeatable)\n"
        "  --press K@F[+L]  hold key K for L frames (default 4) starting at frame F\n"
        "                   K: coin1 coin2 start1 start2 service up down left right b1 b2 b3\n"
        "  --wav FILE       write the mono audio stream to a WAV file\n"
        "  --poke F:ADDR=V  write byte V (hex) to work RAM address ADDR (hex) at frame F; F:+ = every frame from F on\n"
        "  --stage-log PREFIX  log each STAGE banner and write the RAM at that moment (PREFIX_a-b.bin)\n"
        "  --ram-dump F:FILE  write the 64 KiB work RAM after frame F (repeatable)\n"
        "  --nothrottle     run as fast as possible\n"
        "  --autoplay SEED  random button-mashing bot (coins, starts, both players)\n"
        "  --dump-prg FILE  write the 512 KiB 68000 program image and exit\n"
        "  --watch-write HEX  print frame and PC whenever the game writes this RAM address\n"
        "  --ini FILE      settings file (default: bloodbro.ini next to the program); --no-ini ignores it\n"
        "  --cheat-file F   MAME cheat file to use instead of the built-in cheats (default: bloodbro.xml next to the program, if present)\n"
        "  --cheat NAME[=N] switch a cheat on (N = parameter number, 0-based); repeatable\n"
        "  --menu-shot P:F  render menu page P (dip|cheats) into PNG F and exit (UI test)\n"
        "  --shader NAME    video preset: off, scanlines, crt, crtstrong (details in the F4 menu)\n"
        "  --renderer NAME  auto (Vulkan, then OpenGL), vulkan, opengl or sdl (plain, no CRT effects)\n"
        "  --no-gl          same as --renderer sdl\n"
        "  --open-menu P@F  open menu P (dip|cheats|video) at frame F, show it once and quit (UI test)\n"
        "  --bezel FILE     monitor frame picture (PNG with a transparent screen area); --no-bezel ignores bezel files\n"
        "  --integer-scale  scale the picture by whole numbers only (best for scanlines)\n"
        "  --screen-shot F:FILE  save the window picture (with shader) after frame F as PNG\n");
}

bool parse_key(const std::string& k, uint32_t& bit) {
    static const struct { const char* n; uint32_t b; } keys[] = {
        {"coin1", IN_COIN1}, {"coin2", IN_COIN2}, {"start1", IN_START1}, {"start2", IN_START2},
        {"service", IN_SERVICE}, {"up", IN_P1_UP}, {"down", IN_P1_DOWN}, {"left", IN_P1_LEFT},
        {"right", IN_P1_RIGHT}, {"b1", IN_P1_B1}, {"b2", IN_P1_B2}, {"b3", IN_P1_B3},
    };
    for (auto& e : keys)
        if (k == e.n) { bit = e.b; return true; }
    return false;
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(); return false; }
        else if (a == "--rom") { const char* v = need("--rom"); if (!v) return false; o.rom_paths.push_back(v); o.rom_given = true; }
        else if (a == "--play") o.play = true;
        else if (a == "--scale") { const char* v = need("--scale"); if (!v) return false; o.scale = std::max(1, std::atoi(v)); }
        else if (a == "--fullscreen") o.fullscreen = true;
        else if (a == "--linear") o.linear = true;
        else if (a == "--pixel-aspect") o.pixel_aspect = true;
        else if (a == "--nosound") o.nosound = true;
        else if (a == "--pad-debug") o.pad_debug = true;
        else if (a == "--watch-write") { const char* v = need("--watch-write"); if (!v) return false; o.watch_write = (uint32_t)std::strtoul(v, nullptr, 16); }
        else if (a == "--shader") { const char* v = need("--shader"); if (!v) return false; o.shader_preset = v; }
        else if (a == "--no-gl") o.renderer = "sdl";
        else if (a == "--renderer") { const char* v = need("--renderer"); if (!v) return false; o.renderer = v; }
        else if (a == "--integer-scale") o.integer_scale = true;
        else if (a == "--bezel") { const char* v = need("--bezel"); if (!v) return false; o.bezel_file = v; }
        else if (a == "--no-bezel") o.no_bezel = true;
        else if (a == "--render-dump") {
            const char* v = need("--render-dump"); if (!v) return false;
            std::string s = v;
            const size_t col = s.rfind(':');
            if (col == std::string::npos) { std::fprintf(stderr, "--render-dump needs DUMP:OUT.png\n"); return false; }
            o.render_dump_in = s.substr(0, col);
            o.render_dump_out = s.substr(col + 1);
        }
        else if (a == "--open-menu") {
            const char* v = need("--open-menu"); if (!v) return false;
            std::string s = v;
            const size_t at = s.find('@');
            o.open_menu_page = s.substr(0, at);
            o.open_menu_frame = at == std::string::npos ? 0 : std::strtoull(s.c_str() + at + 1, nullptr, 10);
        }
        else if (a == "--screen-shot") {
            const char* v = need("--screen-shot"); if (!v) return false;
            std::string s = v;
            const size_t col = s.find(':');
            if (col == std::string::npos) { std::fprintf(stderr, "--screen-shot needs FRAME:FILE\n"); return false; }
            o.screen_shots.push_back({std::strtoull(s.substr(0, col).c_str(), nullptr, 10), s.substr(col + 1)});
        }
        else if (a == "--ini") { const char* v = need("--ini"); if (!v) return false; o.ini_path = v; }
        else if (a == "--no-ini") o.use_ini = false;
        else if (a == "--cheat-file") { const char* v = need("--cheat-file"); if (!v) return false; o.cheat_file = v; }
        else if (a == "--cheat") {
            const char* v = need("--cheat"); if (!v) return false;
            std::string s = v;
            const size_t eq = s.rfind('=');
            int param = 0;
            if (eq != std::string::npos && eq + 1 < s.size() && std::isdigit((unsigned char)s[eq + 1])) { param = std::atoi(s.c_str() + eq + 1); s = s.substr(0, eq); }
            o.cheats_on.push_back({s, param});
        }
        else if (a == "--menu-shot") {
            const char* v = need("--menu-shot"); if (!v) return false;
            std::string s = v;
            const size_t col = s.find(':');
            if (col == std::string::npos) { std::fprintf(stderr, "--menu-shot needs PAGE:FILE\n"); return false; }
            o.menu_shot_page = s.substr(0, col);
            o.menu_shot_file = s.substr(col + 1);
        }
        else if (a == "--dump-prg") { const char* v = need("--dump-prg"); if (!v) return false; o.dump_prg = v; }
        else if (a == "--autoplay") { const char* v = need("--autoplay"); if (!v) return false; o.autoplay = true; o.autoplay_seed = std::strtoull(v, nullptr, 10); }
        else if (a == "--headless") o.headless = true;
        else if (a == "--nothrottle") o.nothrottle = true;
        else if (a == "--frames") { const char* v = need("--frames"); if (!v) return false; o.frames = std::strtoull(v, nullptr, 10); }
        else if (a == "--dsw") { const char* v = need("--dsw"); if (!v) return false; o.dsw = (uint16_t)std::strtoul(v, nullptr, 16); o.dsw_given = true; }
        else if (a == "--wav") { const char* v = need("--wav"); if (!v) return false; o.wav = v; }
        else if (a == "--shot") {
            const char* v = need("--shot"); if (!v) return false;
            std::string s = v;
            size_t c = s.find(':');
            if (c == std::string::npos) { std::fprintf(stderr, "--shot needs FRAME:FILE\n"); return false; }
            o.shots.push_back({std::strtoull(s.substr(0, c).c_str(), nullptr, 10), s.substr(c + 1)});
        }
        else if (a == "--ram-dump") {
            const char* v = need("--ram-dump"); if (!v) return false;
            std::string s = v;
            size_t c = s.find(':');
            if (c == std::string::npos) { std::fprintf(stderr, "--ram-dump needs FRAME:FILE\n"); return false; }
            o.ram_dumps.push_back({std::strtoull(s.substr(0, c).c_str(), nullptr, 10), s.substr(c + 1)});
        }
        else if (a == "--stage-log") { const char* v = need("--stage-log"); if (!v) return false; o.stage_log = v; }
        else if (a == "--poke") {
            const char* v = need("--poke"); if (!v) return false;
            std::string s = v;
            size_t c1 = s.find(':'), eq = s.find('=');
            if (c1 == std::string::npos || eq == std::string::npos) { std::fprintf(stderr, "--poke needs FRAME[+]:ADDR=VALUE\n"); return false; }
            Options::Poke pk{};
            std::string fr = s.substr(0, c1);
            pk.every_frame = !fr.empty() && fr.back() == '+';
            pk.frame = std::strtoull(fr.c_str(), nullptr, 10);
            pk.addr = (uint32_t)std::strtoul(s.substr(c1 + 1, eq - c1 - 1).c_str(), nullptr, 16);
            pk.value = (uint8_t)std::strtoul(s.substr(eq + 1).c_str(), nullptr, 16);
            o.pokes.push_back(pk);
        }
        else if (a == "--press") {
            const char* v = need("--press"); if (!v) return false;
            std::string s = v;
            size_t at = s.find('@');
            if (at == std::string::npos) { std::fprintf(stderr, "--press needs KEY@FRAME[+LEN]\n"); return false; }
            Press p{};
            if (!parse_key(s.substr(0, at), p.bit)) { std::fprintf(stderr, "unknown key in --press: %s\n", s.c_str()); return false; }
            std::string rest = s.substr(at + 1);
            size_t plus = rest.find('+');
            p.start = std::strtoull(rest.substr(0, plus).c_str(), nullptr, 10);
            p.len = plus == std::string::npos ? 4 : std::strtoull(rest.substr(plus + 1).c_str(), nullptr, 10);
            o.presses.push_back(p);
        }
        else if (!a.empty() && a[0] != '-') { o.rom_paths.push_back(a); o.rom_given = true; }
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(); return false; }
    }
    return true;
}

bool save_png(const std::string& file, const uint32_t* argb, int w, int h) {
    std::vector<uint8_t> rgb((size_t)w * h * 3);
    for (int i = 0; i < w * h; i++) {
        rgb[i * 3 + 0] = (uint8_t)(argb[i] >> 16);
        rgb[i * 3 + 1] = (uint8_t)(argb[i] >> 8);
        rgb[i * 3 + 2] = (uint8_t)argb[i];
    }
    size_t len = 0;
    void* png = tdefl_write_image_to_png_file_in_memory(rgb.data(), w, h, 3, &len);
    if (!png) return false;
    FILE* f = std::fopen(file.c_str(), "wb");
    if (!f) { mz_free(png); return false; }
    std::fwrite(png, 1, len, f);
    std::fclose(f);
    mz_free(png);
    return true;
}

bool save_wav(const std::string& file, const std::vector<int16_t>& s, uint32_t rate) {
    FILE* f = std::fopen(file.c_str(), "wb");
    if (!f) return false;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint32_t bytes = (uint32_t)(s.size() * 2);
    std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(1); w16(1); w32(rate); w32(rate * 2); w16(2); w16(16);
    std::fwrite("data", 1, 4, f); w32(bytes);
    std::fwrite(s.data(), 2, s.size(), f);
    std::fclose(f);
    return true;
}

// Linear-interpolating resampler from the YM3812 rate (~49716 Hz) to the device rate, mono -> stereo.
struct Resampler {
    double step = 1.0;
    double pos = 0.0;
    std::vector<int16_t> in;
    void set_rates(double in_rate, double out_rate) { step = in_rate / out_rate; }
    void process(const std::vector<int16_t>& src, std::vector<int16_t>& out_stereo) {
        in.insert(in.end(), src.begin(), src.end());
        size_t i = (size_t)pos;
        while (i + 1 < in.size()) {
            const double frac = pos - (double)i;
            const int16_t v = (int16_t)(in[i] + (in[i + 1] - in[i]) * frac);
            out_stereo.push_back(v);
            out_stereo.push_back(v);
            pos += step;
            i = (size_t)pos;
        }
        if (i > 0) {
            in.erase(in.begin(), in.begin() + (std::ptrdiff_t)std::min(i, in.size()));
            pos -= (double)i;
        }
    }
};

uint32_t keyboard_state() {
    const bool* k = SDL_GetKeyboardState(nullptr);
    uint32_t b = 0;
    auto set = [&](SDL_Scancode sc, uint32_t bit) { if (k[sc]) b |= bit; };
    set(SDL_SCANCODE_UP, IN_P1_UP); set(SDL_SCANCODE_DOWN, IN_P1_DOWN);
    set(SDL_SCANCODE_LEFT, IN_P1_LEFT); set(SDL_SCANCODE_RIGHT, IN_P1_RIGHT);
    set(SDL_SCANCODE_Z, IN_P1_B1); set(SDL_SCANCODE_X, IN_P1_B2); set(SDL_SCANCODE_C, IN_P1_B3);
    set(SDL_SCANCODE_I, IN_P2_UP); set(SDL_SCANCODE_K, IN_P2_DOWN);
    set(SDL_SCANCODE_J, IN_P2_LEFT); set(SDL_SCANCODE_L, IN_P2_RIGHT);
    set(SDL_SCANCODE_U, IN_P2_B1); set(SDL_SCANCODE_O, IN_P2_B2); set(SDL_SCANCODE_P, IN_P2_B3);
    set(SDL_SCANCODE_1, IN_START1); set(SDL_SCANCODE_2, IN_START2);
    set(SDL_SCANCODE_5, IN_COIN1); set(SDL_SCANCODE_6, IN_COIN2);
    set(SDL_SCANCODE_9, IN_SERVICE);
    return b;
}

// A connected controller: either a recognised SDL gamepad (known mapping) or a raw
// joystick (fallback for pads SDL has no mapping for).
struct Pad {
    SDL_Gamepad* gc = nullptr;
    SDL_Joystick* js = nullptr;
    SDL_JoystickID id = 0;
    bool active() const { return gc || js; }
};

bool g_pad_debug = false;

uint32_t pad_state(const Pad& pad, bool second) {
    if (!pad.active()) return 0;
    bool up = false, dn = false, lf = false, rt = false, b1 = false, b2 = false, b3 = false;
    bool start = false, coin = false;
    constexpr int kDead = 16000;

    if (pad.gc) {
        SDL_Gamepad* g = pad.gc;
        auto btn = [&](SDL_GamepadButton x) { return SDL_GetGamepadButton(g, x); };
        const int ax = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
        const int ay = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
        up = btn(SDL_GAMEPAD_BUTTON_DPAD_UP) || ay < -kDead;
        dn = btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ay > kDead;
        lf = btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || ax < -kDead;
        rt = btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || ax > kDead;
        b1 = btn(SDL_GAMEPAD_BUTTON_SOUTH);    // A / cross
        b2 = btn(SDL_GAMEPAD_BUTTON_EAST);     // B / circle
        b3 = btn(SDL_GAMEPAD_BUTTON_WEST) || btn(SDL_GAMEPAD_BUTTON_NORTH);   // X / Y
        start = btn(SDL_GAMEPAD_BUTTON_START);
        coin = btn(SDL_GAMEPAD_BUTTON_BACK);
    } else {
        SDL_Joystick* j = pad.js;
        const int naxes = SDL_GetNumJoystickAxes(j), nbtn = SDL_GetNumJoystickButtons(j);
        const int nhats = SDL_GetNumJoystickHats(j);
        if (naxes >= 2) {
            const int ax = SDL_GetJoystickAxis(j, 0), ay = SDL_GetJoystickAxis(j, 1);
            lf = ax < -kDead; rt = ax > kDead; up = ay < -kDead; dn = ay > kDead;
        }
        if (nhats >= 1) {
            const Uint8 h = SDL_GetJoystickHat(j, 0);
            if (h & SDL_HAT_UP) up = true;
            if (h & SDL_HAT_DOWN) dn = true;
            if (h & SDL_HAT_LEFT) lf = true;
            if (h & SDL_HAT_RIGHT) rt = true;
        }
        auto b = [&](int i) { return i < nbtn && SDL_GetJoystickButton(j, i); };
        // typical layout: 0..3 = A B X Y / cross circle square triangle; 6,8 = back/select; 7,9 = start
        b1 = b(0); b2 = b(1); b3 = b(2) || b(3);
        coin = b(6) || b(8);
        start = b(7) || b(9);
    }

    uint32_t r = 0;
    if (!second) {
        if (up) r |= IN_P1_UP;
        if (dn) r |= IN_P1_DOWN;
        if (lf) r |= IN_P1_LEFT;
        if (rt) r |= IN_P1_RIGHT;
        if (b1) r |= IN_P1_B1;
        if (b2) r |= IN_P1_B2;
        if (b3) r |= IN_P1_B3;
        if (start) r |= IN_START1;
        if (coin) r |= IN_COIN1;
    } else {
        if (up) r |= IN_P2_UP;
        if (dn) r |= IN_P2_DOWN;
        if (lf) r |= IN_P2_LEFT;
        if (rt) r |= IN_P2_RIGHT;
        if (b1) r |= IN_P2_B1;
        if (b2) r |= IN_P2_B2;
        if (b3) r |= IN_P2_B3;
        if (start) r |= IN_START2;
        if (coin) r |= IN_COIN2;
    }
    return r;
}

// Hotplug handling (SDL3: `id` is the joystick instance id for ADDED and REMOVED events).
void pad_add(Pad* pads, SDL_JoystickID id) {
    int slot = -1;
    for (int i = 0; i < 2; i++) {
        if (pads[i].active() && pads[i].id == id) return;   // already open (gamepad and joystick event for one device)
        if (slot < 0 && !pads[i].active()) slot = i;
    }
    if (slot < 0) return;
    Pad p;
    if (SDL_IsGamepad(id)) {
        p.gc = SDL_OpenGamepad(id);
        if (p.gc) {
            p.id = id;
            const char* name = SDL_GetGamepadName(p.gc);
            std::printf("pad %d: %s (SDL gamepad, mapped)\n", slot + 1, name ? name : "?");
        }
    } else {
        p.js = SDL_OpenJoystick(id);
        if (p.js) {
            p.id = id;
            const char* name = SDL_GetJoystickName(p.js);
            std::printf("pad %d: %s (raw joystick, %d axes %d buttons %d hats: fallback mapping)\n", slot + 1,
                        name ? name : "?", SDL_GetNumJoystickAxes(p.js), SDL_GetNumJoystickButtons(p.js),
                        SDL_GetNumJoystickHats(p.js));
        }
    }
    if (p.active()) pads[slot] = p;
}

void pad_remove(Pad* pads, SDL_JoystickID id) {
    for (int i = 0; i < 2; i++) {
        if (pads[i].active() && pads[i].id == id) {
            if (pads[i].gc) SDL_CloseGamepad(pads[i].gc);
            if (pads[i].js) SDL_CloseJoystick(pads[i].js);
            pads[i] = Pad{};
            std::printf("pad %d disconnected\n", i + 1);
        }
    }
}

// Simple random bot: wanders, mashes fire, throws rolls/dynamite, inserts coins and presses
// start for both players. Not meant to play well, only to push the game through as many code paths
// as possible (attract mode, continue screens, name entry, 2P mode ...).
struct Autoplay {
    uint64_t s = 1;
    uint32_t dir[2] = {0, 0};
    int dir_left[2] = {0, 0};
    int fire_mode[2] = {0, 0};
    int fire_left[2] = {0, 0};
    int roll_left[2] = {0, 0};
    int dyn_left[2] = {0, 0};

    explicit Autoplay(uint64_t seed = 1) : s(seed * 0x9e3779b97f4a7c15ull + 0x1234567ull) {}
    uint32_t rnd() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (uint32_t)(s >> 16);
    }

    uint32_t next(uint64_t frame) {
        static const uint32_t kP1Dirs[9] = {0, IN_P1_UP, IN_P1_DOWN, IN_P1_LEFT, IN_P1_RIGHT,
                                            IN_P1_UP | IN_P1_LEFT, IN_P1_UP | IN_P1_RIGHT,
                                            IN_P1_DOWN | IN_P1_LEFT, IN_P1_DOWN | IN_P1_RIGHT};
        uint32_t bits = 0;
        for (int p = 0; p < 2; p++) {
            if (--dir_left[p] <= 0) {
                dir[p] = rnd() % 9;
                dir_left[p] = 8 + (int)(rnd() % 40);
            }
            if (--fire_left[p] <= 0) {
                fire_mode[p] = (int)(rnd() % 3);  // 0 idle, 1 slow tap, 2 rapid fire
                fire_left[p] = 30 + (int)(rnd() % 60);
            }
            uint32_t b = kP1Dirs[dir[p]];
            const bool fire = fire_mode[p] == 2 ? (frame & 1) : fire_mode[p] == 1 ? ((frame & 7) < 2) : false;
            if (fire) b |= IN_P1_B1;
            if (roll_left[p] > 0) { b |= IN_P1_B2; roll_left[p]--; } else if (rnd() % 45 == 0) roll_left[p] = 3;
            if (dyn_left[p] > 0) { b |= IN_P1_B3; dyn_left[p]--; } else if (rnd() % 90 == 0) dyn_left[p] = 3;
            // player 1 bits -> player 2 bits (same layout shifted)
            if (p == 1) {
                uint32_t q = 0;
                if (b & IN_P1_UP) q |= IN_P2_UP;
                if (b & IN_P1_DOWN) q |= IN_P2_DOWN;
                if (b & IN_P1_LEFT) q |= IN_P2_LEFT;
                if (b & IN_P1_RIGHT) q |= IN_P2_RIGHT;
                if (b & IN_P1_B1) q |= IN_P2_B1;
                if (b & IN_P1_B2) q |= IN_P2_B2;
                if (b & IN_P1_B3) q |= IN_P2_B3;
                b = q;
            }
            bits |= b;
        }
        if (frame % 600 < 4) bits |= IN_COIN1;
        if (frame % 600 >= 300 && frame % 600 < 304) bits |= IN_COIN2;
        if (frame % 240 >= 100 && frame % 240 < 106) bits |= IN_START1;
        if (frame % 480 >= 220 && frame % 480 < 226) bits |= IN_START2;
        return bits;
    }
};


// ---------------------------------------------------------------------------
// Program start-up helpers: console handling, error dialogs, settings
// ---------------------------------------------------------------------------

// The Windows build is a GUI program (no console window). When it is started from a terminal we
// attach to that terminal so command line output still shows up.
void attach_console_if_needed() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            std::freopen("CONOUT$", "w", stdout);
            std::freopen("CONOUT$", "w", stderr);
        }
    }
#endif
}

std::string exe_dir() {
    const char* b = SDL_GetBasePath();   // owned by SDL in SDL3
    return b ? std::string(b) : std::string("./");
}

bool file_exists(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// Reports a start-up problem: always on stderr, and in a message box unless running headless.
int fatal(bool gui, const std::string& title, const std::string& msg) {
    std::fprintf(stderr, "%s: %s\n", title.c_str(), msg.c_str());
    if (gui) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), msg.c_str(), nullptr);
    return 1;
}

bool g_gui = false;   // for errors raised while the game runs (recomp_fatal)

// options that decide where settings come from have to be known before the real parsing
void prescan_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--headless" || a == "--no-ini") o.use_ini = false, o.headless = o.headless || a == "--headless";
        else if (a == "--ini" && i + 1 < argc) o.ini_path = argv[++i];
        else if (a == "--dump-prg" || a == "--menu-shot" || a == "--render-dump") o.use_ini = false;
    }
}

std::string hex4(unsigned v) {
    char b[8];
    std::snprintf(b, sizeof b, "%04X", v & 0xffff);
    return b;
}

std::string cheat_key(const std::string& desc) {
    std::string k = desc;
    for (char& ch : k)
        if (ch == '=' || ch == '\n' || ch == '\r' || ch == ';' || ch == '[' || ch == ']') ch = '-';
    return k;
}

void apply_ini(const Ini& ini, Options& o) {
    o.scale = std::max(1, std::min(8, ini.get_int("video", "scale", o.scale)));
    o.fullscreen = ini.get_bool("video", "fullscreen", o.fullscreen);
    o.nosound = !ini.get_bool("audio", "sound", !o.nosound);
    o.ini_rom = ini.get("game", "rom", "");
    o.ini_cheat_file = ini.get("game", "cheats", "");
    if (ini.has("dip", "dsw")) o.dsw = (uint16_t)std::strtoul(ini.get("dip", "dsw", "FFFF").c_str(), nullptr, 16);
}

// Connects a cheat engine to a machine and loads the cheat file. Returns the number of cheats (or -1).
int attach_cheats(Machine& m, cheat::Engine& eng, const std::string& file, std::string& err, std::string* source) {
    eng.set_memory([&m](int size, uint32_t a) { return (uint64_t)m.cheat_read(size, a); },
                   [&m](int size, uint32_t a, uint64_t v) { m.cheat_write(size, a, (uint32_t)v); });
    m.set_frame_hook([&eng]() { eng.run_frame(); });
    // an external file (next to the program or given on the command line) wins over the built-in list
    if (!file.empty()) {
        const int n = eng.load(file, err);
        if (n >= 0) { if (source) *source = file; return n; }
        std::fprintf(stderr, "cheat file problem: %s\nusing the built-in cheats instead\n", err.c_str());
    }
    std::string xml;
    for (const char* const* p = kBuiltinCheatChunks; *p; ++p) xml += *p;
    if (xml.empty()) { err = "no cheats available"; return -1; }
    if (source) *source = "";
    return eng.load_string(xml, "built-in cheats", err);
}

std::string find_cheat_file(const Options& o, const std::string& base) {
    const std::string names[] = {o.cheat_file, o.ini_cheat_file, base + "bloodbro.xml", base + "cheat/bloodbro.xml",
                                 base + "cheats/bloodbro.xml", "bloodbro.xml", "cheat/bloodbro.xml"};
    for (const std::string& n : names)
        if (!n.empty() && file_exists(n)) return n;
    return std::string();
}

// writes menu overlay + game picture into a PNG (UI test, --menu-shot)
bool save_menu_png(const std::string& file, const uint32_t* game, const uint32_t* overlay) {
    const int W = Menu::kWidth, H = Menu::kHeight;
    std::vector<uint32_t> img((size_t)W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint32_t g = game[(y / 2) * Video::kWidth + x / 2];
            const uint32_t o = overlay[y * W + x];
            const unsigned a = o >> 24;
            auto mix = [&](int sh) { return (((g >> sh) & 0xff) * (255 - a) + ((o >> sh) & 0xff) * a) / 255; };
            img[(size_t)y * W + x] = 0xff000000u | (mix(16) << 16) | (mix(8) << 8) | mix(0);
        }
    return save_png(file, img.data(), W, H);
}

struct Layout {
    SDL_Rect game{0, 0, 0, 0};    // where the game picture goes
    SDL_Rect bezel{0, 0, 0, 0};   // where the bezel picture goes
    bool use_bezel = false;
    bool bezel_on_top = true;
};

std::string find_bezel_file(const Options& o, const Ini& ini, const std::string& base) {
    const std::string names[] = {o.bezel_file, ini.get("video", "bezel_file", ""), base + "bezel/bloodbro.png", base + "bezels/bloodbro.png",
                                 base + "bloodbro_bezel.png", base + "bezel.png", "bezel/bloodbro.png", "bezels/bloodbro.png",
                                 "bloodbro_bezel.png", "bezel.png"};
    for (const std::string& n : names)
        if (!n.empty() && file_exists(n)) return n;
    return std::string();
}

}  // namespace

void recomp_fatal(const std::string& message) {
    fatal(g_gui, "Blood Bros. - recompiled program", message);
    std::exit(1);
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // started without arguments (double-click): the launcher window; it starts the game as "bloodbro.exe --play"
    // (unless the launcher is switched off: then the game starts at once with the settings from bloodbro.ini)
    if (argc <= 1 && !launcher_skipped()) return run_launcher();
#endif
    attach_console_if_needed();

    Options opt;
    prescan_args(argc, argv, opt);
    const std::string base = exe_dir();
    Ini ini;
    const std::string ini_path = opt.ini_path.empty() ? base + "bloodbro.ini" : opt.ini_path;
    if (opt.use_ini && ini.load(ini_path)) apply_ini(ini, opt);
    if (!parse_args(argc, argv, opt)) return 1;
    const bool gui = !opt.headless;
    g_gui = gui;
    VideoSettings vs;
    if (opt.use_ini) vs.load(ini);
    if (opt.linear) vs.filter = 1;
    if (opt.pixel_aspect) vs.aspect = 1;
    if (opt.integer_scale) vs.integer_scale = 1;
    int renderer_choice = vs.renderer;   // --renderer only applies to this run (not saved to bloodbro.ini)
    if (!opt.renderer.empty()) {
        renderer_choice = VideoSettings::renderer_index(opt.renderer);
        if (renderer_choice < 0) return fatal(gui, "Blood Bros. - option error", "Unknown --renderer '" + opt.renderer + "' (auto, vulkan, opengl, sdl).");
    }
    Bezel bezel;   // optional monitor frame; used when vs.bezel is on
    if (gui && !opt.no_bezel) {
        const std::string bf = find_bezel_file(opt, ini, base);
        std::string berr;
        if (!bf.empty()) {
            if (bezel.load(bf, berr)) std::printf("bezel: %s (%dx%d, %s)\n", bf.c_str(), bezel.w, bezel.h, bezel.has_window ? "with screen window" : "no transparent window: used as background");
            else std::fprintf(stderr, "bezel not used: %s\n", berr.c_str());
        }
    }
    if (!opt.shader_preset.empty() && !vs.apply_preset_name(opt.shader_preset))
        return fatal(gui, "Blood Bros. - option error", "Unknown --shader preset '" + opt.shader_preset + "' (off, scanlines, crt, crtstrong).");
    if (!opt.menu_shot_page.empty() && opt.frames == 0) opt.frames = 120;

    // ---- find the ROM ----
    std::vector<std::string> searched;
    if (!opt.rom_given) {
        const std::string cands[] = {opt.ini_rom, base + "roms/bloodbro.zip", base + "bloodbro.zip", "roms/bloodbro.zip", "bloodbro.zip"};
        for (const std::string& cnd : cands) {
            if (cnd.empty()) continue;
            searched.push_back(cnd);
            if (file_exists(cnd)) { opt.rom_paths.assign(1, cnd); break; }
        }
    }
    if (opt.rom_paths.empty()) {
        std::string msg = "The ROM set bloodbro.zip was not found.\n\nPut bloodbro.zip next to bloodbro.exe or into a folder named "
                          "\"roms\" next to it, or drag the zip file onto bloodbro.exe.\n\nLooked in:";
        for (const std::string& s : searched) msg += "\n  " + s;
        return fatal(gui, "Blood Bros. - ROM not found", msg);
    }

    RomSet roms;
    std::string err, warn;
    if (!load_romset(opt.rom_paths, roms, err, warn)) {
        return fatal(gui, "Blood Bros. - ROM problem",
                     err + "\n\nFile: " + opt.rom_paths[0] + "\n\nThe ROM set must be the MAME set \"bloodbro\" (bloodbro.zip).");
    }
    if (!warn.empty()) std::fputs(warn.c_str(), stderr);
    std::printf("loaded: %s (%s)\n", roms.set_name.c_str(), roms.description.c_str());

    if (!opt.render_dump_in.empty()) {  // draw a saved RAM state (see --ram-dump) with the built-in renderer
        std::vector<uint8_t> ram(0x10000);
        FILE* f = std::fopen(opt.render_dump_in.c_str(), "rb");
        if (!f || std::fread(ram.data(), 1, ram.size(), f) != ram.size()) return fatal(false, "render-dump", "cannot read " + opt.render_dump_in);
        std::fclose(f);
        unsigned sc[7] = {0, 0, 0, 0, 0, 0, 0};
        if (FILE* g = std::fopen((opt.render_dump_in + ".crtc").c_str(), "r")) {
            if (std::fscanf(g, "%x %x %x %x %x %x %x", &sc[0], &sc[1], &sc[2], &sc[3], &sc[4], &sc[5], &sc[6]) != 7) std::fprintf(stderr, "bad .crtc file\n");
            std::fclose(g);
        }
        const uint16_t scroll[6] = {(uint16_t)sc[0], (uint16_t)sc[1], (uint16_t)sc[2], (uint16_t)sc[3], (uint16_t)sc[4], (uint16_t)sc[5]};
        Video video;
        video.init(roms);
        std::vector<uint32_t> img((size_t)Video::kWidth * Video::kHeight);
        video.render(ram.data(), scroll, (uint16_t)sc[6], img.data());
        return save_png(opt.render_dump_out, img.data(), Video::kWidth, Video::kHeight) ? 0 : 1;
    }

    if (!opt.dump_prg.empty()) {
        if (FILE* f = std::fopen(opt.dump_prg.c_str(), "wb")) {
            std::fwrite(roms.maincpu.data(), 1, roms.maincpu.size(), f);
            std::fclose(f);
            std::printf("wrote %s (%zu bytes)\n", opt.dump_prg.c_str(), roms.maincpu.size());
            return 0;
        }
        std::fprintf(stderr, "cannot write %s\n", opt.dump_prg.c_str());
        return 1;
    }

    Autoplay bot(opt.autoplay_seed);

    auto machine_owner = std::make_unique<Machine>();  // large (framebuffers): keep off the stack
    Machine& machine = *machine_owner;
    if (!machine.init(roms))
        return fatal(gui, "Blood Bros. - recompiled program not available", "Cannot run the recompiled program: " + machine.init_error() + ".");
    std::printf("cpu: %s\n", machine.cpu_name());
    machine.set_dsw(opt.dsw);
    machine.set_watch_write(opt.watch_write);

    // ---- cheats (MAME cheat file) ----
    cheat::Engine cheats;
    std::string cheat_err;
    std::string cheat_path = find_cheat_file(opt, base);   // empty: use the built-in list
    const int n_cheats = attach_cheats(machine, cheats, cheat_path, cheat_err, &cheat_path);
    if (n_cheats >= 0) std::printf("cheats: %d entries from %s\n", n_cheats, cheat_path.empty() ? "the built-in list" : cheat_path.c_str());
    else std::fprintf(stderr, "cheats unavailable: %s\n", cheat_err.c_str());
    if (n_cheats >= 0 && opt.use_ini)
        for (const auto& kv : ini.items("cheats")) {  // restore the switches saved by the menu
            for (size_t i = 0; i < cheats.cheats().size(); i++) {
                if (cheats.cheats()[i].separator || cheat_key(cheats.cheats()[i].desc) != kv.first) continue;
                const int param = kv.second.find(',') != std::string::npos ? std::atoi(kv.second.c_str() + kv.second.find(',') + 1) : 0;
                cheats.set_param((int)i, param);
                cheats.set_enabled((int)i, !kv.second.empty() && kv.second[0] == '1');
            }
        }
    for (const auto& on : opt.cheats_on) {  // --cheat NAME[=N]
        const int idx = cheats.find(on.first);
        if (idx < 0) { std::fprintf(stderr, "unknown cheat: %s\n", on.first.c_str()); continue; }
        cheats.set_param(idx, on.second);
        cheats.set_enabled(idx, true);
    }

    // ---- SDL setup ----
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_Texture* overlay_tex = nullptr;
    SDL_Texture* bezel_tex = nullptr;
    std::unique_ptr<GpuRenderer> gpu;    // Vulkan or OpenGL with the CRT shader; null: plain SDL renderer
    SDL_AudioStream* audio = nullptr;    // SDL3: a stream bound to the default playback device
    Pad pads[2];
    const int audio_rate = 48000;        // the stream converts to whatever the device uses
    Resampler resampler;
    const bool vsync = !opt.use_ini || ini.get_bool("video", "vsync", true);

    g_pad_debug = opt.pad_debug;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");  // keep pad input when the window is unfocused
    SDL_InitFlags init_flags = SDL_INIT_EVENTS;
    if (!opt.headless)
        init_flags |= SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | (opt.nosound ? 0 : SDL_INIT_AUDIO);
    if (!SDL_Init(init_flags)) return fatal(gui, "Blood Bros. - SDL error", std::string("SDL could not start: ") + SDL_GetError());

    if (!opt.headless) {
        const double start_aspect = vs.aspect == 1 ? (double)Video::kWidth / Video::kHeight : 4.0 / 3.0;
        int ww = Video::kWidth * opt.scale;
        int wh = (int)(ww / start_aspect);
        if (bezel.valid() && vs.bezel && bezel.has_window) {   // the game window inside the frame gets the usual size
            ww = (int)(Video::kWidth * opt.scale * (double)bezel.w / bezel.win_w + 0.5);
            wh = (int)(ww * (double)bezel.h / bezel.w + 0.5);
        }
        // hidden until a renderer works, so a failed Vulkan attempt does not flash a window
        const SDL_WindowFlags wflags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | (opt.fullscreen ? SDL_WINDOW_FULLSCREEN : 0);

        // GPU renderers in order of preference (CRT shader); if none works, the plain SDL renderer
        std::vector<RendererApi> apis;
        if (renderer_choice == 0 || renderer_choice == 1) apis.push_back(RendererApi::Vulkan);
        if (renderer_choice == 0 || renderer_choice == 2) apis.push_back(RendererApi::OpenGL);
        if (renderer_choice == 1 || renderer_choice == 2) {   // explicitly chosen: the other API as fallback
            apis.push_back(renderer_choice == 1 ? RendererApi::OpenGL : RendererApi::Vulkan);
        }
        for (RendererApi api : apis) {
            std::unique_ptr<GpuRenderer> r = make_renderer(api);
            const char* api_name = api == RendererApi::Vulkan ? "Vulkan" : "OpenGL";
            if (!r) {
                std::fprintf(stderr, "%s: not built into this program\n", api_name);
                continue;
            }
            renderer_prepare_window(api);
            window = SDL_CreateWindow("Blood Bros.", ww, wh, wflags | renderer_window_flag(api));
            if (!window) {
                std::fprintf(stderr, "%s window not possible (%s)\n", api_name, SDL_GetError());
                continue;
            }
            std::string gerr;
            if (r->init(window, vsync, gerr)) {
                gpu = std::move(r);
                break;
            }
            std::fprintf(stderr, "%s not usable (%s)\n", api_name, gerr.c_str());
            r.reset();
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        if (gpu) {
            if (bezel.valid()) gpu->set_bezel(bezel.argb.data(), bezel.w, bezel.h);
            std::printf("video: %s (%s)\n", gpu->api_name(), gpu->info().c_str());
        } else {
            if (renderer_choice != 3) std::fprintf(stderr, "no Vulkan or OpenGL: using the plain renderer, no CRT effects\n");
            window = SDL_CreateWindow("Blood Bros.", ww, wh, wflags);
            if (!window) return fatal(gui, "Blood Bros. - window error", std::string("Cannot create the window: ") + SDL_GetError());
            renderer = SDL_CreateRenderer(window, nullptr);
            if (!renderer) renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
            if (!renderer) return fatal(gui, "Blood Bros. - video error", std::string("Cannot create the renderer: ") + SDL_GetError());
            SDL_SetRenderVSync(renderer, vsync ? 1 : 0);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, Video::kWidth, Video::kHeight);
            if (!texture) return fatal(gui, "Blood Bros. - video error", std::string("Cannot create the screen texture: ") + SDL_GetError());
            SDL_SetTextureScaleMode(texture, vs.filter == 1 ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
            overlay_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, Menu::kWidth, Menu::kHeight);
            if (overlay_tex) {
                SDL_SetTextureBlendMode(overlay_tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(overlay_tex, SDL_SCALEMODE_NEAREST);
            }
            if (bezel.valid()) {
                bezel_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, bezel.w, bezel.h);
                if (bezel_tex) {
                    SDL_UpdateTexture(bezel_tex, nullptr, bezel.argb.data(), bezel.w * 4);
                    SDL_SetTextureBlendMode(bezel_tex, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureScaleMode(bezel_tex, SDL_SCALEMODE_LINEAR);   // smooth when the frame is shown smaller than the image
                }
            }
            const char* rname = SDL_GetRendererName(renderer);
            std::printf("video: SDL renderer (%s), no CRT effects\n", rname ? rname : "?");
        }
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_ShowWindow(window);

        // pads are opened from the SDL_EVENT_*_ADDED events SDL queues at start-up (and on hotplug)

        if (!opt.nosound) {
            const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, audio_rate};
            audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);   // starts paused
            if (!audio) std::fprintf(stderr, "audio disabled: %s\n", SDL_GetError());
        }
    }
    resampler.set_rates(Machine::audio_rate(), audio_rate);

    const double frame_time = 1.0 / Machine::kRefreshHz;
    const Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 next_frame = SDL_GetPerformanceCounter();
    const size_t bytes_per_frame = (size_t)(audio_rate / Machine::kRefreshHz) * 4;
    bool audio_started = false;
    bool fullscreen = opt.fullscreen;

    // ---- settings file and menus ----
    uint16_t dsw = opt.dsw;
    auto save_settings = [&]() {
        if (!opt.use_ini || opt.headless) return;
        ini.set_int("video", "scale", opt.scale);
        ini.set_bool("video", "fullscreen", fullscreen);
        vs.save(ini);
        ini.set_bool("video", "vsync", ini.get_bool("video", "vsync", true));
        ini.set_bool("audio", "sound", !opt.nosound);
        if (!opt.rom_paths.empty()) ini.set("game", "rom", opt.rom_paths[0]);
        ini.set("game", "cheats", cheat_path);   // empty: built-in list
        ini.set("dip", "dsw", hex4(dsw));
        ini.clear_section("cheats");
        for (const auto& ch : cheats.cheats())
            if (!ch.separator)
                ini.set("cheats", cheat_key(ch.desc), std::string(ch.enabled ? "1" : "0") + (ch.items.empty() ? "" : "," + std::to_string(ch.param_index)));
        ini.save(ini_path, "; bloodbro.ini - settings of bloodbro.exe. Saved by the game (F1 = DIP switches, F2 = cheats).");
    };

    std::function<void()> mh_video_changed = [&]() {
        if (!gpu && texture) SDL_SetTextureScaleMode(texture, vs.filter == 1 ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        save_settings();
    };

    Menu menu;
    std::vector<uint32_t> overlay_px((size_t)Menu::kWidth * Menu::kHeight, 0u);
    {
        Menu::Hooks mh;
        mh.get_dsw = [&]() { return dsw; };
        mh.set_dsw = [&](uint16_t v) { dsw = v; machine.set_dsw(v); };
        mh.reset_game = [&]() { machine.reset(); };
        mh.settings_changed = save_settings;
        mh.cheats = n_cheats >= 0 ? &cheats : nullptr;
        mh.cheat_file_note = "No cheats available in this build.";
        mh.video = &vs;
        mh.shader_available = gpu != nullptr;
        mh.bezel_available = bezel.valid();
        mh.video_changed = [&]() { if (mh_video_changed) mh_video_changed(); };
        menu.set_hooks(mh);
        cheats.on_change = save_settings;
    }
    bool menu_was_active = false;

    // where the game picture goes inside the window: aspect ratio, integer scaling
    auto compute_dst = [&](int ow, int oh) -> SDL_Rect {
        if (vs.aspect == 2) return SDL_Rect{0, 0, ow, oh};
        const double aspect = vs.aspect == 1 ? (double)Video::kWidth / Video::kHeight : 4.0 / 3.0;
        int dw = ow, dh = (int)(ow / aspect);
        if (dh > oh) { dh = oh; dw = (int)(oh * aspect); }
        if (vs.integer_scale) {
            const double w0 = Video::kHeight * aspect;
            const int k = std::min(oh / Video::kHeight, (int)(ow / w0));
            if (k >= 1) { dh = Video::kHeight * k; dw = (int)(w0 * k + 0.5); }
        }
        return SDL_Rect{(ow - dw) / 2, (oh - dh) / 2, dw, dh};
    };
    // game picture and bezel positions inside the window
    auto compute_layout = [&](int ow, int oh) -> Layout {
        Layout L;
        if (bezel.valid() && vs.bezel) {
            const double s = std::min((double)ow / bezel.w, (double)oh / bezel.h);
            L.bezel = SDL_Rect{(int)((ow - bezel.w * s) / 2), (int)((oh - bezel.h * s) / 2), (int)(bezel.w * s + 0.5), (int)(bezel.h * s + 0.5)};
            L.use_bezel = true;
            if (bezel.has_window) {
                L.game = SDL_Rect{L.bezel.x + (int)(bezel.win_x * s + 0.5), L.bezel.y + (int)(bezel.win_y * s + 0.5),
                                  (int)(bezel.win_w * s + 0.5), (int)(bezel.win_h * s + 0.5)};
                L.bezel_on_top = true;
            } else {  // a background picture: the game sits in front of it, as large as it fits
                const SDL_Rect g = compute_dst(L.bezel.w, L.bezel.h);
                L.game = SDL_Rect{L.bezel.x + g.x, L.bezel.y + g.y, g.w, g.h};
                L.bezel_on_top = false;
            }
            return L;
        }
        L.game = compute_dst(ow, oh);
        return L;
    };
    uint64_t shot_frame = ~0ull;  // frame number of the picture being presented (for --screen-shot)
    auto want_screen_shot = [&]() -> const Shot* {
        for (const Shot& s : opt.screen_shots)
            if (s.frame == shot_frame) return &s;
        return nullptr;
    };

    auto present = [&]() {
        int ow = 0, oh = 0;
        if (gpu) SDL_GetWindowSizeInPixels(window, &ow, &oh);
        else if (renderer) SDL_GetCurrentRenderOutputSize(renderer, &ow, &oh);
        else return;
        const Layout lay = compute_layout(ow, oh);
        const SDL_Rect dst = lay.game;
        const uint32_t* ov = nullptr;
        if (menu.active()) {
            menu.render(overlay_px.data());
            ov = overlay_px.data();
        }
        const Shot* shot = want_screen_shot();
        if (gpu) {
            gpu->draw(machine.framebuffer(), Video::kWidth, Video::kHeight, vs, ow, oh, dst, ov, Menu::kWidth, Menu::kHeight,
                      lay.use_bezel ? &lay.bezel : nullptr, lay.bezel_on_top);
            if (shot) {
                std::vector<uint32_t> px;
                int w, h;
                if (gpu->read_pixels(px, w, h) && save_png(shot->file, px.data(), w, h)) std::printf("saved %s (window, frame %llu)\n", shot->file.c_str(), (unsigned long long)shot->frame);
            }
            gpu->present();
            return;
        }
        auto frect = [](const SDL_Rect& r) { return SDL_FRect{(float)r.x, (float)r.y, (float)r.w, (float)r.h}; };
        const SDL_FRect fdst = frect(dst), fbezel = frect(lay.bezel);
        SDL_UpdateTexture(texture, nullptr, machine.framebuffer(), Video::kWidth * 4);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (lay.use_bezel && bezel_tex && !lay.bezel_on_top) SDL_RenderTexture(renderer, bezel_tex, nullptr, &fbezel);
        SDL_RenderTexture(renderer, texture, nullptr, &fdst);
        if (lay.use_bezel && bezel_tex && lay.bezel_on_top) SDL_RenderTexture(renderer, bezel_tex, nullptr, &fbezel);
        if (ov && overlay_tex) {
            SDL_UpdateTexture(overlay_tex, nullptr, ov, Menu::kWidth * 4);
            SDL_RenderTexture(renderer, overlay_tex, nullptr, &fdst);
        }
        if (shot) {
            bool saved = false;
            if (SDL_Surface* s = SDL_RenderReadPixels(renderer, nullptr)) {
                if (SDL_Surface* c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ARGB8888)) {
                    std::vector<uint32_t> px((size_t)c->w * c->h);
                    for (int y = 0; y < c->h; y++)
                        std::memcpy(&px[(size_t)y * c->w], (const uint8_t*)c->pixels + (size_t)y * c->pitch, (size_t)c->w * 4);
                    saved = save_png(shot->file, px.data(), c->w, c->h);
                    SDL_DestroySurface(c);
                }
                SDL_DestroySurface(s);
            }
            if (saved) std::printf("saved %s (window, frame %llu)\n", shot->file.c_str(), (unsigned long long)shot->frame);
        }
        SDL_RenderPresent(renderer);
    };

    std::vector<int16_t> frame_audio, wav_audio, stereo;
    bool quit = false;
    bool prev_f3 = false;

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = true;
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED) pad_add(pads, e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) pad_remove(pads, e.gdevice.which);
            else if (e.type == SDL_EVENT_JOYSTICK_ADDED) {
                if (!SDL_IsGamepad(e.jdevice.which)) pad_add(pads, e.jdevice.which);  // unmapped pad: raw fallback
            } else if (e.type == SDL_EVENT_JOYSTICK_REMOVED) pad_remove(pads, e.jdevice.which);
            else if (g_pad_debug && e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN)
                std::printf("joy %d button %d down\n", (int)e.jbutton.which, (int)e.jbutton.button);
            else if (g_pad_debug && e.type == SDL_EVENT_JOYSTICK_AXIS_MOTION && std::abs(e.jaxis.value) > 16000)
                std::printf("joy %d axis %d = %d\n", (int)e.jaxis.which, (int)e.jaxis.axis, (int)e.jaxis.value);
            else if (g_pad_debug && e.type == SDL_EVENT_JOYSTICK_HAT_MOTION)
                std::printf("joy %d hat %d = %d\n", (int)e.jhat.which, (int)e.jhat.hat, (int)e.jhat.value);
            else if (e.type == SDL_EVENT_KEY_DOWN && menu.active()) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_UP) menu.key(Menu::Key::Up);
                else if (k == SDLK_DOWN) menu.key(Menu::Key::Down);
                else if (k == SDLK_LEFT) menu.key(Menu::Key::Left);
                else if (k == SDLK_RIGHT) menu.key(Menu::Key::Right);
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) menu.key(Menu::Key::Enter);
                else if (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) menu.key(Menu::Key::Back);
                else if (!e.key.repeat && k == SDLK_F1) menu.toggle(Menu::Page::Dip);
                else if (!e.key.repeat && k == SDLK_F2) menu.toggle(Menu::Page::Cheats);
                else if (!e.key.repeat && k == SDLK_F4) menu.toggle(Menu::Page::Video);
                else if (!e.key.repeat && k == SDLK_F3) machine.reset();
            }
            else if (menu.active() && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                switch (e.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP: menu.key(Menu::Key::Up); break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN: menu.key(Menu::Key::Down); break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT: menu.key(Menu::Key::Left); break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: menu.key(Menu::Key::Right); break;
                case SDL_GAMEPAD_BUTTON_SOUTH: menu.key(Menu::Key::Enter); break;
                case SDL_GAMEPAD_BUTTON_EAST: menu.key(Menu::Key::Back); break;
                default: break;
                }
            }
            else if (menu.active() && e.type == SDL_EVENT_JOYSTICK_HAT_MOTION && !SDL_GetGamepadFromID(e.jhat.which)) {
                if (e.jhat.value & SDL_HAT_UP) menu.key(Menu::Key::Up);
                else if (e.jhat.value & SDL_HAT_DOWN) menu.key(Menu::Key::Down);
                else if (e.jhat.value & SDL_HAT_LEFT) menu.key(Menu::Key::Left);
                else if (e.jhat.value & SDL_HAT_RIGHT) menu.key(Menu::Key::Right);
            }
            else if (menu.active() && e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN && !SDL_GetGamepadFromID(e.jbutton.which)) {
                if (e.jbutton.button == 0) menu.key(Menu::Key::Enter);
                else if (e.jbutton.button == 1) menu.key(Menu::Key::Back);
            }
            else if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                const SDL_Keycode k = e.key.key;
                if (k == SDLK_F1 && window) menu.toggle(Menu::Page::Dip);
                else if (k == SDLK_F2 && window) menu.toggle(Menu::Page::Cheats);
                else if (k == SDLK_F4 && window) menu.toggle(Menu::Page::Video);
                else if (k == SDLK_ESCAPE) quit = true;
                else if (k == SDLK_F10 && window) {
                    vs.filter = (vs.filter + 1) % 3;   // sharp -> linear -> nearest
                    if (mh_video_changed) mh_video_changed();
                } else if (k == SDLK_F11 && window) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(window, fullscreen);   // borderless desktop fullscreen
                }
            }
        }

        if (!opt.open_menu_page.empty() && !menu.active() && machine.frame_count() == opt.open_menu_frame)
            menu.open(opt.open_menu_page == "cheats" ? Menu::Page::Cheats : opt.open_menu_page == "video" ? Menu::Page::Video : Menu::Page::Dip);

        // ---- menu open: the game is paused, only the picture and the menu are drawn ----
        if (menu.active()) {
            if (!menu_was_active && audio) {
                SDL_PauseAudioStreamDevice(audio);
                SDL_ClearAudioStream(audio);
                audio_started = false;
            }
            menu_was_active = true;
            shot_frame = machine.frame_count();
            present();
            shot_frame = ~0ull;
            if (!opt.open_menu_page.empty()) { quit = true; continue; }
            SDL_Delay(16);
            continue;
        }
        if (menu_was_active) {
            menu_was_active = false;
            next_frame = SDL_GetPerformanceCounter();
        }

        // ---- inputs ----
        uint32_t in = 0;
        if (!opt.headless) {
            in = keyboard_state() | pad_state(pads[0], false) | pad_state(pads[1], true);
            const bool f3 = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F3] != 0;
            if (f3 && !prev_f3) machine.reset();
            prev_f3 = f3;
        }
        const uint64_t fr = machine.frame_count();
        for (const Press& p : opt.presses)
            if (fr >= p.start && fr < p.start + p.len) in |= p.bit;
        if (opt.autoplay) in |= bot.next(fr);
        machine.set_inputs(in);

        for (const auto& pk : opt.pokes)
            if (pk.every_frame ? fr >= pk.frame : fr == pk.frame) machine.poke8(pk.addr, pk.value);

        // ---- emulate one frame ----
        machine.run_frame();

        frame_audio.clear();
        machine.take_audio(frame_audio);
        if (!opt.wav.empty()) wav_audio.insert(wav_audio.end(), frame_audio.begin(), frame_audio.end());

        // frame numbers in --shot/--press count completed frames (first frame = 0)
        for (const Shot& s : opt.shots)
            if (s.frame == fr) {
                if (save_png(s.file, machine.framebuffer(), Video::kWidth, Video::kHeight))
                    std::printf("saved %s (frame %llu)\n", s.file.c_str(), (unsigned long long)fr);
                else
                    std::fprintf(stderr, "cannot write %s\n", s.file.c_str());
            }

        if (!opt.stage_log.empty()) {
            // the banner is ASCII text in the text layer, row 13: "STAGE a-b"
            const uint8_t* ram = machine.work_ram();
            auto ch = [&](int col) { return ram[0xd800 + (13 * 32 + col) * 2 + 1]; };
            static bool shown = false;
            const bool banner = ch(12) == 'S' && ch(13) == 'T' && ch(14) == 'A' && ch(15) == 'G' && ch(16) == 'E';
            if (banner && !shown) {
                const char a = (char)ch(18), b = (char)ch(20);
                std::printf("frame %llu: STAGE %c-%c\n", (unsigned long long)fr, a, b);
                std::string fn = opt.stage_log + "_" + a + "-" + b + "_" + std::to_string(fr) + ".bin";
                if (FILE* f = std::fopen(fn.c_str(), "wb")) { std::fwrite(ram, 1, 0x10000, f); std::fclose(f); }
            }
            shown = banner;
        }
        for (const Shot& s : opt.ram_dumps)
            if (s.frame == fr)
                {
                    if (FILE* f = std::fopen(s.file.c_str(), "wb")) {
                        std::fwrite(machine.work_ram(), 1, 0x10000, f);
                        std::fclose(f);
                    }
                    if (FILE* f = std::fopen((s.file + ".crtc").c_str(), "w")) {  // scroll registers and layer mask
                        const uint16_t* sc = machine.scroll_regs();
                        std::fprintf(f, "%04x %04x %04x %04x %04x %04x %04x\n", sc[0], sc[1], sc[2], sc[3], sc[4], sc[5], machine.layer_enable());
                        std::fclose(f);
                    }
                }

        shot_frame = fr;
        present();
        shot_frame = ~0ull;

        // ---- audio output & pacing ----
        if (audio) {
            stereo.clear();
            resampler.process(frame_audio, stereo);
            SDL_PutAudioStreamData(audio, stereo.data(), (int)(stereo.size() * sizeof(int16_t)));
            auto queued = [&]() { return (size_t)std::max(0, SDL_GetAudioStreamQueued(audio)); };
            if (!audio_started && queued() >= bytes_per_frame * 3) {
                SDL_ResumeAudioStreamDevice(audio);
                audio_started = true;
            }
            if (!opt.nothrottle)
                while (queued() > bytes_per_frame * 4 && !quit) SDL_Delay(1);
        } else if (!opt.nothrottle && !opt.headless) {
            next_frame += (Uint64)(frame_time * (double)freq);
            const Uint64 now = SDL_GetPerformanceCounter();
            if (next_frame > now) SDL_Delay((Uint32)((next_frame - now) * 1000 / freq));
            else if (now - next_frame > freq / 5) next_frame = now;  // fell behind: resync
        }

        if (opt.frames && machine.frame_count() >= opt.frames) quit = true;
    }

    if (!opt.menu_shot_page.empty()) {  // UI test: draw the requested menu page over the last frame
        menu.open(opt.menu_shot_page == "cheats" ? Menu::Page::Cheats : opt.menu_shot_page == "video" ? Menu::Page::Video : Menu::Page::Dip);
        menu.render(overlay_px.data());
        if (save_menu_png(opt.menu_shot_file, machine.framebuffer(), overlay_px.data()))
            std::printf("saved %s\n", opt.menu_shot_file.c_str());
    }
    save_settings();

    std::printf("%s\n", machine.cpu_report().c_str());

    if (const char* d = std::getenv("BB_DUMP_RAM")) {  // debug: dump work RAM + CRTC state
        std::string base = d;
        if (FILE* f = std::fopen((base + ".ram").c_str(), "wb")) { std::fwrite(machine.work_ram(), 1, 0x10000, f); std::fclose(f); }
        if (FILE* f = std::fopen((base + ".txt").c_str(), "w")) {
            const uint16_t* s = machine.scroll_regs();
            std::fprintf(f, "scroll %04x %04x %04x %04x %04x %04x layer_en %04x\n", s[0], s[1], s[2], s[3], s[4], s[5], machine.layer_enable());
            std::fclose(f);
        }
    }

    if (!opt.wav.empty()) {
        if (save_wav(opt.wav, wav_audio, (uint32_t)std::lround(Machine::audio_rate())))
            std::printf("wrote %s (%zu samples)\n", opt.wav.c_str(), wav_audio.size());
    }

    if (audio) SDL_DestroyAudioStream(audio);
    for (Pad& p : pads) {
        if (p.gc) SDL_CloseGamepad(p.gc);
        if (p.js) SDL_CloseJoystick(p.js);
    }
    gpu.reset();  // while the window still exists
    if (bezel_tex) SDL_DestroyTexture(bezel_tex);
    if (overlay_tex) SDL_DestroyTexture(overlay_tex);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
