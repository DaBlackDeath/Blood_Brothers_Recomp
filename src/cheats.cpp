#include "cheats.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cheat {

// ===========================================================================
// Minimal XML reader (elements, attributes, text, comments, entities)
// ===========================================================================
namespace {

struct XNode {
    std::string name;
    std::map<std::string, std::string> attrs;
    std::string text;
    std::vector<XNode> kids;
};

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string decode_entities(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { out += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 10) { out += s[i]; continue; }
        const std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "amp") out += '&';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            const long v = ent[1] == 'x' ? std::strtol(ent.c_str() + 2, nullptr, 16) : std::strtol(ent.c_str() + 1, nullptr, 10);
            if (v > 0 && v < 128) out += (char)v;
        } else { out += s.substr(i, e - i + 1); }
        i = e;
    }
    return out;
}

class XmlParser {
public:
    explicit XmlParser(const std::string& s) : s_(s) {}

    bool parse(XNode& root, std::string& err) {
        skip_misc();
        if (!element(root, err)) return false;
        return true;
    }

private:
    bool at(const char* lit) const { return s_.compare(p_, std::strlen(lit), lit) == 0; }
    void skip_ws() { while (p_ < s_.size() && std::isspace((unsigned char)s_[p_])) p_++; }

    // skips whitespace, comments, <?...?> and <!DOCTYPE ...>
    void skip_misc() {
        for (;;) {
            skip_ws();
            if (at("<!--")) {
                size_t e = s_.find("-->", p_ + 4);
                p_ = e == std::string::npos ? s_.size() : e + 3;
            } else if (at("<?")) {
                size_t e = s_.find("?>", p_);
                p_ = e == std::string::npos ? s_.size() : e + 2;
            } else if (at("<!")) {
                size_t e = s_.find('>', p_);
                p_ = e == std::string::npos ? s_.size() : e + 1;
            } else {
                break;
            }
        }
    }

    bool element(XNode& n, std::string& err) {
        if (p_ >= s_.size() || s_[p_] != '<') { err = "expected '<'"; return false; }
        p_++;
        size_t b = p_;
        while (p_ < s_.size() && !std::isspace((unsigned char)s_[p_]) && s_[p_] != '>' && s_[p_] != '/') p_++;
        n.name = s_.substr(b, p_ - b);
        // attributes
        for (;;) {
            skip_ws();
            if (p_ >= s_.size()) { err = "unexpected end inside <" + n.name + ">"; return false; }
            if (s_[p_] == '/') {
                if (!at("/>")) { err = "bad tag end"; return false; }
                p_ += 2;
                return true;  // self-closing
            }
            if (s_[p_] == '>') { p_++; break; }
            size_t k = p_;
            while (p_ < s_.size() && s_[p_] != '=' && !std::isspace((unsigned char)s_[p_]) && s_[p_] != '>' && s_[p_] != '/') p_++;
            std::string key = s_.substr(k, p_ - k);
            skip_ws();
            std::string val;
            if (p_ < s_.size() && s_[p_] == '=') {
                p_++;
                skip_ws();
                if (p_ < s_.size() && (s_[p_] == '"' || s_[p_] == '\'')) {
                    const char q = s_[p_++];
                    size_t e = s_.find(q, p_);
                    if (e == std::string::npos) { err = "unterminated attribute"; return false; }
                    val = decode_entities(s_.substr(p_, e - p_));
                    p_ = e + 1;
                } else { err = "attribute without quotes"; return false; }
            }
            n.attrs[key] = val;
        }
        // content
        for (;;) {
            if (p_ >= s_.size()) { err = "missing </" + n.name + ">"; return false; }
            if (at("<!--")) { skip_misc(); continue; }
            if (at("<![CDATA[")) {
                size_t e = s_.find("]]>", p_);
                if (e == std::string::npos) { err = "unterminated CDATA"; return false; }
                n.text += s_.substr(p_ + 9, e - p_ - 9);
                p_ = e + 3;
                continue;
            }
            if (at("</")) {
                size_t e = s_.find('>', p_);
                if (e == std::string::npos) { err = "bad closing tag"; return false; }
                p_ = e + 1;
                return true;
            }
            if (s_[p_] == '<') {
                n.kids.emplace_back();
                if (!element(n.kids.back(), err)) return false;
                continue;
            }
            size_t e = s_.find('<', p_);
            if (e == std::string::npos) e = s_.size();
            n.text += decode_entities(s_.substr(p_, e - p_));
            p_ = e;
        }
    }

    const std::string& s_;
    size_t p_ = 0;
};

}  // namespace

// ===========================================================================
// Expressions
// ===========================================================================
enum class T { Num, Var, Mem, Un, Bin, Assign, Comma };
enum Op { ADD, SUB, MUL, DIV, MOD, SHL, SHR, LT, LE, GT, GE, EQ, NE, BAND, BXOR, BOR, LAND, LOR, NEG, PLUS, BNOT, LNOT };

struct Node {
    T t = T::Num;
    int64_t num = 0;
    std::string name;
    int op = 0;
    int size = 1;
    std::unique_ptr<Node> a, b;
};

namespace {

struct Token {
    enum K { Num, Id, Op, End } k = End;
    int64_t num = 0;
    std::string text;
};

bool all_hex(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}

std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

bool tokenize(const std::string& s, std::vector<Token>& out, std::string& err) {
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace((unsigned char)c)) { i++; continue; }
        if (c == '#') {  // decimal
            size_t b = ++i;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) i++;
            Token t; t.k = Token::Num; t.num = std::strtoll(s.substr(b, i - b).c_str(), nullptr, 10);
            out.push_back(t);
            continue;
        }
        if (c == '$') {
            size_t b = ++i;
            while (i < s.size() && std::isxdigit((unsigned char)s[i])) i++;
            Token t; t.k = Token::Num; t.num = (int64_t)std::strtoull(s.substr(b, i - b).c_str(), nullptr, 16);
            out.push_back(t);
            continue;
        }
        if (std::isalnum((unsigned char)c) || c == '_') {
            size_t b = i;
            while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.')) i++;
            std::string w = s.substr(b, i - b);
            Token t;
            if (w.size() > 2 && (w[0] == '0') && (w[1] == 'x' || w[1] == 'X')) {
                t.k = Token::Num; t.num = (int64_t)std::strtoull(w.c_str() + 2, nullptr, 16);
            } else if (all_hex(w)) {
                t.k = Token::Num; t.num = (int64_t)std::strtoull(w.c_str(), nullptr, 16);  // hex is the default base
            } else {
                const std::string u = upper(w);
                static const std::pair<const char*, const char*> words[] = {
                    {"BAND", "&"}, {"BOR", "|"}, {"BXOR", "^"}, {"LAND", "&&"}, {"LOR", "||"}, {"LT", "<"},
                    {"LE", "<="}, {"GT", ">"}, {"GE", ">="}, {"EQ", "=="}, {"NE", "!="}, {"BNOT", "~"}, {"LNOT", "!"}};
                bool word = false;
                for (auto& wd : words)
                    if (u == wd.first) { t.k = Token::Op; t.text = wd.second; word = true; break; }
                if (!word) { t.k = Token::Id; t.text = w; }
            }
            out.push_back(t);
            continue;
        }
        static const char* ops2[] = {"<<", ">>", "<=", ">=", "==", "!=", "&&", "||"};
        bool done = false;
        for (const char* o : ops2)
            if (s.compare(i, 2, o) == 0) { Token t; t.k = Token::Op; t.text = o; out.push_back(t); i += 2; done = true; break; }
        if (done) continue;
        if (std::strchr("+-*/%<>&|^~!=(),@", c)) {
            Token t; t.k = Token::Op; t.text = std::string(1, c);
            out.push_back(t);
            i++;
            continue;
        }
        err = std::string("unexpected character '") + c + "'";
        return false;
    }
    out.emplace_back();  // End
    return true;
}

class ExprParser {
public:
    ExprParser(std::vector<Token>& t) : t_(t) {}
    std::unique_ptr<Node> parse(std::string& err) {
        auto n = comma();
        if (!n) { err = err_; return nullptr; }
        if (t_[p_].k != Token::End) { err = "unexpected '" + t_[p_].text + "'"; return nullptr; }
        return n;
    }

private:
    bool is_op(const char* o) const { return t_[p_].k == Token::Op && t_[p_].text == o; }

    std::unique_ptr<Node> fail(const std::string& m) { err_ = m; return nullptr; }

    std::unique_ptr<Node> comma() {
        auto n = assign();
        while (n && is_op(",")) {
            p_++;
            auto r = assign();
            if (!r) return nullptr;
            auto c = std::make_unique<Node>();
            c->t = T::Comma; c->a = std::move(n); c->b = std::move(r);
            n = std::move(c);
        }
        return n;
    }

    std::unique_ptr<Node> assign() {
        auto l = binary(1);
        if (!l) return nullptr;
        if (is_op("=")) {
            if (l->t != T::Var && l->t != T::Mem) return fail("cannot assign to this expression");
            p_++;
            auto r = assign();
            if (!r) return nullptr;
            auto n = std::make_unique<Node>();
            n->t = T::Assign; n->a = std::move(l); n->b = std::move(r);
            return n;
        }
        return l;
    }

    static int prec(const std::string& o, int& op) {
        static const struct { const char* s; int p; int op; } tab[] = {
            {"*", 10, MUL}, {"/", 10, DIV}, {"%", 10, MOD}, {"+", 9, ADD}, {"-", 9, SUB}, {"<<", 8, SHL}, {">>", 8, SHR},
            {"<", 7, LT}, {"<=", 7, LE}, {">", 7, GT}, {">=", 7, GE}, {"==", 6, EQ}, {"!=", 6, NE}, {"&", 5, BAND},
            {"^", 4, BXOR}, {"|", 3, BOR}, {"&&", 2, LAND}, {"||", 1, LOR}};
        for (auto& e : tab)
            if (o == e.s) { op = e.op; return e.p; }
        return 0;
    }

    std::unique_ptr<Node> binary(int min_prec) {
        auto l = unary();
        while (l && t_[p_].k == Token::Op) {
            int op = 0;
            const int pr = prec(t_[p_].text, op);
            if (pr == 0 || pr < min_prec) break;
            p_++;
            auto r = binary(pr + 1);
            if (!r) return nullptr;
            auto n = std::make_unique<Node>();
            n->t = T::Bin; n->op = op; n->a = std::move(l); n->b = std::move(r);
            l = std::move(n);
        }
        return l;
    }

    std::unique_ptr<Node> unary() {
        const Token& t = t_[p_];
        if (t.k == Token::Op && (t.text == "-" || t.text == "+" || t.text == "~" || t.text == "!")) {
            p_++;
            auto a = unary();
            if (!a) return nullptr;
            auto n = std::make_unique<Node>();
            n->t = T::Un;
            n->op = t.text == "-" ? NEG : t.text == "+" ? PLUS : t.text == "~" ? BNOT : LNOT;
            n->a = std::move(a);
            return n;
        }
        return primary();
    }

    std::unique_ptr<Node> primary() {
        const Token t = t_[p_];
        if (t.k == Token::Num) {
            p_++;
            auto n = std::make_unique<Node>();
            n->num = t.num;
            return n;
        }
        if (t.k == Token::Id) {
            p_++;
            // memory access: space.type@address
            if (is_op("@")) {
                const size_t dot = t.text.find('.');
                if (dot == std::string::npos || dot + 1 >= t.text.size()) return fail("bad memory accessor '" + t.text + "'");
                if (t.text.substr(0, dot) != "maincpu") return fail("only maincpu memory is available ('" + t.text + "')");
                const char last = (char)std::tolower((unsigned char)t.text.back());
                int size = last == 'b' ? 1 : last == 'w' ? 2 : last == 'd' ? 4 : last == 'q' ? 4 : 0;
                if (!size) return fail("bad memory accessor '" + t.text + "'");
                p_++;  // '@'
                auto addr = unary();
                if (!addr) return nullptr;
                auto n = std::make_unique<Node>();
                n->t = T::Mem; n->size = size; n->a = std::move(addr);
                return n;
            }
            auto n = std::make_unique<Node>();
            n->t = T::Var; n->name = t.text;
            return n;
        }
        if (is_op("(")) {
            p_++;
            auto n = comma();
            if (!n) return nullptr;
            if (!is_op(")")) return fail("missing ')'");
            p_++;
            return n;
        }
        return fail(t.k == Token::End ? "unexpected end of expression" : "unexpected '" + t.text + "'");
    }

    std::vector<Token>& t_;
    size_t p_ = 0;
    std::string err_;
};

int64_t eval_node(const Node& n, Context& c);

void assign_to(const Node& lhs, int64_t v, Context& c) {
    if (lhs.t == T::Var) {
        if (c.vars) (*c.vars)[lhs.name] = v;
    } else if (lhs.t == T::Mem) {
        const uint32_t addr = (uint32_t)eval_node(*lhs.a, c);
        if (c.write) c.write(lhs.size, addr, (uint64_t)v);
    }
}

int64_t eval_node(const Node& n, Context& c) {
    switch (n.t) {
    case T::Num: return n.num;
    case T::Var: {
        if (!c.vars) return 0;
        auto it = c.vars->find(n.name);
        return it == c.vars->end() ? 0 : it->second;
    }
    case T::Mem: {
        const uint32_t addr = (uint32_t)eval_node(*n.a, c);
        return c.read ? (int64_t)c.read(n.size, addr) : 0;
    }
    case T::Un: {
        const int64_t a = eval_node(*n.a, c);
        switch (n.op) {
        case NEG: return -a;
        case PLUS: return a;
        case BNOT: return ~a;
        default: return !a;
        }
    }
    case T::Assign: {
        const int64_t v = eval_node(*n.b, c);
        assign_to(*n.a, v, c);
        return v;
    }
    case T::Comma:
        eval_node(*n.a, c);
        return eval_node(*n.b, c);
    case T::Bin: {
        if (n.op == LAND) return eval_node(*n.a, c) && eval_node(*n.b, c);
        if (n.op == LOR) return eval_node(*n.a, c) || eval_node(*n.b, c);
        const int64_t a = eval_node(*n.a, c), b = eval_node(*n.b, c);
        switch (n.op) {
        case ADD: return a + b;
        case SUB: return a - b;
        case MUL: return a * b;
        case DIV: return b ? a / b : 0;
        case MOD: return b ? a % b : 0;
        case SHL: return b >= 0 && b < 64 ? (int64_t)((uint64_t)a << b) : 0;
        case SHR: return b >= 0 && b < 64 ? (int64_t)((uint64_t)a >> b) : 0;
        case LT: return a < b;
        case LE: return a <= b;
        case GT: return a > b;
        case GE: return a >= b;
        case EQ: return a == b;
        case NE: return a != b;
        case BAND: return a & b;
        case BXOR: return a ^ b;
        default: return a | b;
        }
    }
    }
    return 0;
}

}  // namespace

Expr::Expr() = default;
Expr::~Expr() = default;
Expr::Expr(Expr&&) noexcept = default;
Expr& Expr::operator=(Expr&&) noexcept = default;

bool Expr::parse(const std::string& text, std::string& error) {
    std::vector<Token> toks;
    if (!tokenize(text, toks, error)) return false;
    ExprParser p(toks);
    root_ = p.parse(error);
    return root_ != nullptr;
}

int64_t Expr::eval(Context& ctx) const { return root_ ? eval_node(*root_, ctx) : 0; }

// ===========================================================================
// Cheat file and engine
// ===========================================================================
void Engine::set_memory(std::function<uint64_t(int, uint32_t)> read, std::function<void(int, uint32_t, uint64_t)> write) {
    read_ = std::move(read);
    write_ = std::move(write);
}

static bool read_file_text(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    if (n > 0 && std::fread(&out[0], 1, (size_t)n, f) != (size_t)n) { std::fclose(f); return false; }
    std::fclose(f);
    return true;
}

static void parse_script(const XNode& sn, Script& sc, std::string& cheat_error) {
    for (const XNode& a : sn.kids) {
        if (a.name != "action") continue;  // <output> etc. are ignored
        Action act;
        std::string err;
        auto c = a.attrs.find("condition");
        if (c != a.attrs.end() && !trim(c->second).empty()) {
            act.has_condition = true;
            if (!act.condition.parse(c->second, err)) cheat_error = "condition: " + err;
        }
        if (!act.expr.parse(a.text, err)) cheat_error = "action '" + trim(a.text) + "': " + err;
        sc.actions.push_back(std::move(act));
    }
}

int Engine::load(const std::string& path, std::string& error) {
    std::string text;
    if (!read_file_text(path, text)) { error = "cannot read " + path; return -1; }
    return load_string(text, path, error);
}

int Engine::load_string(const std::string& text, const std::string& path, std::string& error) {
    XNode root;
    XmlParser xp(text);
    if (!xp.parse(root, error)) { error = path + ": " + error; return -1; }
    if (root.name != "mamecheat") { error = path + ": not a MAME cheat file"; return -1; }

    cheats_.clear();
    path_ = path;
    for (const XNode& cn : root.kids) {
        if (cn.name != "cheat") continue;
        Cheat c;
        auto d = cn.attrs.find("desc");
        c.desc = d == cn.attrs.end() ? "" : trim(d->second);
        c.separator = c.desc.empty();
        for (const XNode& k : cn.kids) {
            if (k.name == "comment") {
                c.comment = trim(k.text);
            } else if (k.name == "parameter") {
                std::string err;
                for (const XNode& it : k.kids) {
                    if (it.name != "item") continue;
                    Item item;
                    Expr e;
                    auto v = it.attrs.find("value");
                    if (v == it.attrs.end() || !e.parse(v->second, err)) { c.error = "parameter value: " + err; continue; }
                    Context ctx;
                    item.value = e.eval(ctx);
                    item.label = trim(it.text);
                    c.items.push_back(item);
                }
                if (c.items.empty() && k.attrs.count("max")) {
                    auto num = [&](const char* key, int64_t def) {
                        auto f = k.attrs.find(key);
                        if (f == k.attrs.end()) return def;
                        Expr e;
                        std::string er;
                        Context ctx;
                        return e.parse(f->second, er) ? e.eval(ctx) : def;
                    };
                    const int64_t mn = num("min", 0), mx = num("max", 0);
                    int64_t st = std::max<int64_t>(1, num("step", 1));
                    if ((mx - mn) / st > 255) st = (mx - mn) / 255 + 1;
                    for (int64_t v = mn; v <= mx; v += st) c.items.push_back({v, std::to_string(v)});
                }
            } else if (k.name == "script") {
                auto st = k.attrs.find("state");
                const std::string state = st == k.attrs.end() ? "run" : st->second;
                Script* target = state == "on" ? &c.on : state == "off" ? &c.off : state == "change" ? &c.change : &c.run;
                parse_script(k, *target, c.error);
            }
        }
        cheats_.push_back(std::move(c));
    }
    return (int)cheats_.size();
}

int Engine::find(const std::string& desc) const {
    for (size_t i = 0; i < cheats_.size(); i++)
        if (!cheats_[i].separator && cheats_[i].desc == desc) return (int)i;
    return -1;
}

Context Engine::context(Cheat& c) {
    Context ctx;
    ctx.read = [this](int size, uint32_t a) { return read_ ? read_(size, a) : 0; };
    ctx.write = [this](int size, uint32_t a, uint64_t v) { if (write_) write_(size, a, v); };
    ctx.vars = &c.vars;
    return ctx;
}

void Engine::run_script(Cheat& c, const Script& s) {
    if (s.actions.empty()) return;
    if (!c.items.empty()) c.vars["param"] = c.items[(size_t)std::min<int>(c.param_index, (int)c.items.size() - 1)].value;
    Context ctx = context(c);
    for (const Action& a : s.actions) {
        if (a.has_condition && !a.condition.eval(ctx)) continue;
        a.expr.eval(ctx);
    }
}

void Engine::set_enabled(int index, bool on) {
    if (index < 0 || index >= (int)cheats_.size()) return;
    Cheat& c = cheats_[(size_t)index];
    if (c.separator || !c.error.empty() || c.enabled == on) return;
    c.enabled = on;
    run_script(c, on ? c.on : c.off);
    if (on_change) on_change();
}

void Engine::set_param(int index, int param_index) {
    if (index < 0 || index >= (int)cheats_.size()) return;
    Cheat& c = cheats_[(size_t)index];
    if (c.items.empty()) return;
    param_index = std::max(0, std::min(param_index, (int)c.items.size() - 1));
    if (param_index == c.param_index) return;
    c.param_index = param_index;
    if (c.enabled) run_script(c, c.change);
    if (on_change) on_change();
}

void Engine::run_frame() {
    for (Cheat& c : cheats_)
        if (c.enabled) run_script(c, c.run);
}

void Engine::disable_all() {
    for (size_t i = 0; i < cheats_.size(); i++)
        if (cheats_[i].enabled) set_enabled((int)i, false);
}

}  // namespace cheat
