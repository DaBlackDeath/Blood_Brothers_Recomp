// Cheats in the MAME cheat file format (bloodbro.xml from mamecheat.co.uk or the MAME "cheat" folder).
//
// Supported subset of the format:
//   <cheat desc="...">              a cheat; an empty/blank desc is a separator line in the menu
//     <comment>...</comment>        hint shown in the menu
//     <parameter> <item value="0x01">Label</item> ... </parameter>      choose one of several values
//     <parameter min="0" max="99" step="1"/>                              numeric range
//     <script state="on|off|run|change"> <action condition="expr">expr</action> ... </script>
//   expressions: hex numbers by default ("99" = 0x99), 0x.. / $.. hex, #.. decimal, + - * / % << >>,
//   comparisons, & | ^ ~ ! && ||, BAND BOR BXOR LAND LOR, assignment (=), variables temp0..temp9 and
//   param, and memory access  maincpu.pb@ADDR  (also pw, pd, mb, mw, md: byte/word/long, big-endian)
//
// Memory writes below 0x80000 patch the program ROM (this is how code cheats such as "Invincibility"
// work); writes to 0x80000-0x8ffff change work RAM; everything else is ignored.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cheat {

// ---- expressions ----
struct Node;
struct Context {
    std::function<uint64_t(int size, uint32_t addr)> read;
    std::function<void(int size, uint32_t addr, uint64_t v)> write;
    std::map<std::string, int64_t>* vars = nullptr;
};

class Expr {
public:
    Expr();
    ~Expr();
    Expr(Expr&&) noexcept;
    Expr& operator=(Expr&&) noexcept;
    // Returns false (and sets `error`) if the text cannot be parsed.
    bool parse(const std::string& text, std::string& error);
    bool valid() const { return root_ != nullptr; }
    int64_t eval(Context& ctx) const;

private:
    std::unique_ptr<Node> root_;
};

// ---- cheats ----
struct Action {
    Expr condition;
    bool has_condition = false;
    Expr expr;
};

struct Script {
    std::vector<Action> actions;
};

struct Item {
    int64_t value = 0;
    std::string label;
};

struct Cheat {
    std::string desc;
    std::string comment;
    bool separator = false;
    std::string error;           // non-empty: the cheat could not be understood
    std::vector<Item> items;     // parameter choices (empty: no parameter)
    Script on, off, run, change;
    bool enabled = false;
    int param_index = 0;
    std::map<std::string, int64_t> vars;
};

class Engine {
public:
    // Memory access is given by the host (Machine).
    void set_memory(std::function<uint64_t(int, uint32_t)> read, std::function<void(int, uint32_t, uint64_t)> write);

    // Loads a cheat file; returns the number of cheats (separators included), or -1 on failure.
    int load(const std::string& path, std::string& error);
    // The same from text in memory (used for the cheat list that is built into the program). `name`
    // is only used in messages.
    int load_string(const std::string& xml, const std::string& name, std::string& error);
    const std::string& path() const { return path_; }

    std::vector<Cheat>& cheats() { return cheats_; }
    int find(const std::string& desc) const;

    void set_enabled(int index, bool on);
    void set_param(int index, int param_index);
    void run_frame();   // call once per frame: runs the "run" script of every enabled cheat
    void disable_all();

    // called after enabling/disabling or changing a parameter (for saving settings)
    std::function<void()> on_change;

private:
    void run_script(Cheat& c, const Script& s);
    Context context(Cheat& c);

    std::vector<Cheat> cheats_;
    std::string path_;
    std::function<uint64_t(int, uint32_t)> read_;
    std::function<void(int, uint32_t, uint64_t)> write_;
};

}  // namespace cheat
