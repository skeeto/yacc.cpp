// yacc.cpp - a portable C++20 yacc/bison-compatible parser generator.
//
// Single-file implementation. The output is a self-contained C source
// (plus optional .h) that emulates Bison's yacc.c skeleton interface:
// global int yylex(void), int yyparse(void), YYSTYPE yylval,
// void yyerror(const char*), token #defines, etc.
//
// Implementation outline:
//   1. Lexer for .y files (UTF-8 byte-stream; ASCII keywords)
//   2. Parser builds a Grammar (tokens, rules, prologue, epilogue)
//   3. LALR(1) construction:
//        - augment grammar
//        - LR(0) automaton via closure/goto
//        - per-state, per-kernel-item lookahead via spontaneous + propagation
//   4. Conflict resolution by precedence/associativity
//   5. Code emission with simple row displacement (yypact/yytable/yycheck)

#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "platform.hpp"

namespace yacc {

using std::string;
using std::string_view;
using std::vector;

// ============================================================================
// Utilities
// ============================================================================

struct YaccError : public std::exception {
    std::string msg;
    explicit YaccError(std::string m) : msg(std::move(m)) {}
    const char* what() const noexcept override { return msg.c_str(); }
};

[[noreturn]] static void fatal(const std::string& msg) {
    throw YaccError(msg);
}

// Append-only string builder.  Mirrors a tiny subset of std::ostream's
// "<< operator chains" style without dragging in <iostream>.
struct Buf {
    std::string& s;
    Buf& operator<<(std::string_view v)       { s.append(v); return *this; }
    Buf& operator<<(const std::string& v)     { s.append(v); return *this; }
    Buf& operator<<(const char* v)            { s.append(v); return *this; }
    Buf& operator<<(char v)                   { s.push_back(v); return *this; }
    Buf& operator<<(int v)                    { s.append(std::to_string(v)); return *this; }
    Buf& operator<<(unsigned v)               { s.append(std::to_string(v)); return *this; }
    Buf& operator<<(long v)                   { s.append(std::to_string(v)); return *this; }
    Buf& operator<<(unsigned long v)          { s.append(std::to_string(v)); return *this; }
    Buf& operator<<(long long v)              { s.append(std::to_string(v)); return *this; }
    Buf& operator<<(unsigned long long v)     { s.append(std::to_string(v)); return *this; }
};

template <class... Args>
[[noreturn]] static void fatalf(std::format_string<Args...> fmt, Args&&... args) {
    fatal(std::format(fmt, std::forward<Args>(args)...));
}

static std::string load_input_file(const std::string& path) {
    auto r = read_file(path);
    if (!r.ok) fatalf("cannot open input file '{}'", path);
    return std::move(r.content);
}

// Locale-free, ASCII-only character predicates.
static inline bool ch_isspace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
static inline bool ch_isdigit(unsigned char c) { return c >= '0' && c <= '9'; }
static inline bool ch_isalpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static inline bool ch_isalnum(unsigned char c) { return ch_isalpha(c) || ch_isdigit(c); }
static inline bool ch_isidstart(unsigned char c) { return ch_isalpha(c) || c == '_'; }
static inline bool ch_isidcont(unsigned char c) { return ch_isalnum(c) || c == '_' || c == '.'; }
static inline bool ch_isxdigit(unsigned char c) {
    return ch_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// ============================================================================
// Grammar data model
// ============================================================================

enum class Assoc : uint8_t { None = 0, Left, Right, NonAssoc, Precedence };

struct Symbol {
    string name;
    string display;
    bool is_terminal = false;
    bool is_nullable = false;
    int code = -1;            // External token code (for terminals only)
    int prec = 0;             // 0 = unset
    Assoc assoc = Assoc::None;
    string type_tag;
    bool defined = false;
    bool used = false;
    int alias_of = -1;        // string-literal alias points to canonical symbol
};

struct Production {
    int lhs = -1;
    vector<int> rhs;
    string action;
    int action_line = 0;
    int prec = 0;
    int prec_sym = -1;
    Assoc assoc = Assoc::None;
    int line = 0;
    string lhs_name;
    vector<string> rhs_names;
    string lhs_tag;
    vector<string> rhs_tags;
    // For mid-rule synthetic productions: number of parent rhs symbols already
    // shifted before this synthetic. Action's $k refers to parent's stack.
    int midrule_offset = -1;
    // For parent rules: the rhs_tags reflects ALL slots, including synthetic
    // mid-rule placeholders. midrule_tags[k] = lhs_tag of synthetic at slot k+1
    // (only used to expose typed refs across mid-rule boundary; usually unused).
};

struct Grammar {
    vector<Symbol> syms;
    vector<Production> prods;
    int start_sym = -1;
    int eof_sym = -1;
    int error_sym = -1;
    int undef_sym = -1;
    int accept_sym = -1;

    string prologue;
    string prologue_requires;
    string prologue_provides;
    string prologue_top;
    string epilogue;
    string union_body;
    bool has_union = false;
    string api_value_type;
    string api_value_union_name;
    bool want_locations = false;
    string api_prefix;
    string token_prefix;
    int expected_sr = -1;
    int expected_rr = -1;

    std::unordered_map<string, int> by_name;
    string source_file;

    int find(const string& nm) const {
        auto it = by_name.find(nm);
        return it == by_name.end() ? -1 : it->second;
    }

    int intern(const string& name, bool is_terminal) {
        auto it = by_name.find(name);
        if (it != by_name.end()) return it->second;
        Symbol s;
        s.name = name;
        s.display = name;
        s.is_terminal = is_terminal;
        int idx = (int)syms.size();
        by_name[name] = idx;
        syms.push_back(std::move(s));
        return idx;
    }
};

struct Options {
    string output_file;
    string header_file;
    string defines_path;
    string file_prefix;
    string name_prefix;
    bool want_header = false;
    bool yacc_compat = false;
    bool no_lines = false;
    bool token_table = false;
    bool verbose = false;
    bool debug = false;
};

// ============================================================================
// Lexer
// ============================================================================

enum class Tok : uint16_t {
    EndOfFile,
    Identifier,
    Identifier_Colon,
    CharLit,
    StrLit,
    Int,
    PercentToken, PercentLeft, PercentRight, PercentNonassoc, PercentPrecedence,
    PercentType, PercentStart, PercentUnion,
    PercentExpect, PercentExpectRR,
    PercentDefine, PercentCode, PercentEmpty, PercentPrec, PercentDPrec, PercentMerge,
    PercentNamePrefix, PercentDestructor, PercentPrinter,
    PercentLocations, PercentInitialAction,
    PercentLanguage, PercentSkeleton, PercentDebug,
    PercentDefines, PercentHeader,
    PercentParseParam, PercentLexParam,
    PercentRequire, PercentVerbose, PercentYacc,
    PercentPureParser, PercentGlrParser,
    PercentToken_Table, PercentNoLines, PercentOutput, PercentFilePrefix,
    PercentParam,
    PercentBraces,
    BraceBlock,
    BracketName,
    Tag,
    DoublePercent,
    Or, Semi, Colon,
    Punct,
};

struct Token {
    Tok kind = Tok::EndOfFile;
    string text;
    int line = 1;
    int col = 1;
    long long ival = 0;
    int char_value = -1;
};

class Lexer {
public:
    Lexer(string src, string filename) : src_(std::move(src)), file_(std::move(filename)) {}
    int line() const { return line_; }
    const string& filename() const { return file_; }

    string take_to_end() { string s = src_.substr(pos_); pos_ = src_.size(); return s; }

    Token next_grammar() {
        skip_ws();
        if (pos_ >= src_.size()) return Token{Tok::EndOfFile, "", line_, col_};
        int sline = line_, scol = col_;
        unsigned char c = (unsigned char)src_[pos_];

        if (c == '%' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '%') {
            advance(); advance();
            return Token{Tok::DoublePercent, "%%", sline, scol};
        }
        if (c == '%' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '{') {
            advance(); advance();
            int sl = line_;
            string body = read_until_percent_close(sl);
            Token t{Tok::PercentBraces, std::move(body), sline, scol};
            t.ival = sl;
            return t;
        }
        if (c == '%') {
            advance();
            string id;
            while (pos_ < src_.size() &&
                   (ch_isidcont((unsigned char)src_[pos_]) || src_[pos_] == '-')) {
                id.push_back(src_[pos_]); advance();
            }
            return percent_keyword(id, sline, scol);
        }
        if (c == '|') { advance(); return Token{Tok::Or, "|", sline, scol}; }
        if (c == ';') { advance(); return Token{Tok::Semi, ";", sline, scol}; }
        if (c == ':') { advance(); return Token{Tok::Colon, ":", sline, scol}; }
        if (c == '{') {
            int sl = line_;
            string body = read_brace_block(sl);
            Token t{Tok::BraceBlock, std::move(body), sline, scol};
            t.ival = sl;
            return t;
        }
        if (c == '<') {
            advance();
            string tag;
            while (pos_ < src_.size() && src_[pos_] != '>') {
                tag.push_back(src_[pos_]); advance();
            }
            if (pos_ >= src_.size()) fatalf("unterminated <tag> at line {}", sline);
            advance();
            return Token{Tok::Tag, std::move(tag), sline, scol};
        }
        if (c == '[') {
            advance();
            string nm;
            while (pos_ < src_.size() && src_[pos_] != ']') {
                nm.push_back(src_[pos_]); advance();
            }
            if (pos_ >= src_.size()) fatalf("unterminated [name] at line {}", sline);
            advance();
            return Token{Tok::BracketName, std::move(nm), sline, scol};
        }
        if (c == '\'') return read_char_literal(sline, scol);
        if (c == '"') return read_string_literal(sline, scol);
        if (ch_isdigit(c)) {
            string n;
            while (pos_ < src_.size() && ch_isdigit((unsigned char)src_[pos_])) {
                n.push_back(src_[pos_]); advance();
            }
            Token t{Tok::Int, n, sline, scol};
            t.ival = std::strtoll(n.c_str(), nullptr, 10);
            return t;
        }
        if (ch_isidstart(c)) {
            string id;
            while (pos_ < src_.size() && ch_isidcont((unsigned char)src_[pos_])) {
                id.push_back(src_[pos_]); advance();
            }
            // Look ahead for ':', possibly with an intervening [name].
            size_t save = pos_; int sl = line_, sc = col_;
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == '[') {
                while (pos_ < src_.size() && src_[pos_] != ']') advance();
                if (pos_ < src_.size()) advance();
                skip_ws();
            }
            bool is_lhs = (pos_ < src_.size() && src_[pos_] == ':');
            pos_ = save; line_ = sl; col_ = sc;
            return Token{is_lhs ? Tok::Identifier_Colon : Tok::Identifier,
                         std::move(id), sline, scol};
        }
        // Unknown char: skip silently
        string s(1, (char)c);
        advance();
        Token t{Tok::Punct, std::move(s), sline, scol};
        t.char_value = c;
        return t;
    }

private:
    void advance() {
        if (pos_ >= src_.size()) return;
        if (src_[pos_] == '\n') { line_++; col_ = 1; }
        else col_++;
        pos_++;
    }

    void skip_ws() {
        while (pos_ < src_.size()) {
            unsigned char c = (unsigned char)src_[pos_];
            if (ch_isspace(c)) advance();
            else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n') advance();
            } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
                advance(); advance();
                while (pos_ + 1 < src_.size() && !(src_[pos_] == '*' && src_[pos_ + 1] == '/'))
                    advance();
                if (pos_ + 1 < src_.size()) { advance(); advance(); }
            } else break;
        }
    }

    string read_until_percent_close(int& start_line) {
        start_line = line_;
        std::string out;
        while (pos_ + 1 < src_.size()) {
            if (src_[pos_] == '%' && src_[pos_ + 1] == '}') {
                advance(); advance(); return out;
            }
            out.push_back(src_[pos_]); advance();
        }
        fatalf("unterminated %{{ block starting at line {}", start_line);
    }

    string read_brace_block(int& start_line) {
        start_line = line_;
        int depth = 0;
        std::string out;
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == '{') {
                depth++;
                if (depth > 1) out.push_back(c);
                advance();
            } else if (c == '}') {
                depth--;
                if (depth == 0) { advance(); return out; }
                out.push_back(c); advance();
            } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                while (pos_ < src_.size() && src_[pos_] != '\n') {
                    out.push_back(src_[pos_]); advance();
                }
            } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
                out.push_back(src_[pos_]); advance();
                out.push_back(src_[pos_]); advance();
                while (pos_ + 1 < src_.size() &&
                       !(src_[pos_] == '*' && src_[pos_ + 1] == '/')) {
                    out.push_back(src_[pos_]); advance();
                }
                if (pos_ + 1 < src_.size()) {
                    out.push_back(src_[pos_]); advance();
                    out.push_back(src_[pos_]); advance();
                }
            } else if (c == '\'' || c == '"') {
                char quote = c;
                out.push_back(c); advance();
                while (pos_ < src_.size() && src_[pos_] != quote) {
                    if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) {
                        out.push_back(src_[pos_]); advance();
                    }
                    out.push_back(src_[pos_]); advance();
                }
                if (pos_ < src_.size()) { out.push_back(src_[pos_]); advance(); }
            } else {
                out.push_back(c); advance();
            }
        }
        fatalf("unterminated brace block starting at line {}", start_line);
    }

    Token percent_keyword(const string& kw, int sline, int scol) {
        struct Entry { const char* name; Tok t; };
        static const Entry kws[] = {
            {"token", Tok::PercentToken},
            {"left", Tok::PercentLeft},
            {"right", Tok::PercentRight},
            {"nonassoc", Tok::PercentNonassoc},
            {"precedence", Tok::PercentPrecedence},
            {"type", Tok::PercentType},
            {"start", Tok::PercentStart},
            {"union", Tok::PercentUnion},
            {"expect", Tok::PercentExpect},
            {"expect-rr", Tok::PercentExpectRR},
            {"define", Tok::PercentDefine},
            {"code", Tok::PercentCode},
            {"empty", Tok::PercentEmpty},
            {"prec", Tok::PercentPrec},
            {"dprec", Tok::PercentDPrec},
            {"merge", Tok::PercentMerge},
            {"name-prefix", Tok::PercentNamePrefix},
            {"destructor", Tok::PercentDestructor},
            {"printer", Tok::PercentPrinter},
            {"locations", Tok::PercentLocations},
            {"initial-action", Tok::PercentInitialAction},
            {"language", Tok::PercentLanguage},
            {"skeleton", Tok::PercentSkeleton},
            {"debug", Tok::PercentDebug},
            {"defines", Tok::PercentDefines},
            {"header", Tok::PercentHeader},
            {"parse-param", Tok::PercentParseParam},
            {"lex-param", Tok::PercentLexParam},
            {"require", Tok::PercentRequire},
            {"verbose", Tok::PercentVerbose},
            {"yacc", Tok::PercentYacc},
            {"pure-parser", Tok::PercentPureParser},
            {"glr-parser", Tok::PercentGlrParser},
            {"token-table", Tok::PercentToken_Table},
            {"no-lines", Tok::PercentNoLines},
            {"output", Tok::PercentOutput},
            {"file-prefix", Tok::PercentFilePrefix},
            {"param", Tok::PercentParam},
        };
        for (auto& e : kws) if (kw == e.name) return Token{e.t, kw, sline, scol};
        fatalf("unknown directive %{} at line {}", kw, sline);
    }

    Token read_char_literal(int sline, int scol) {
        advance();
        string raw;
        int value = -1;
        if (pos_ < src_.size() && src_[pos_] == '\\') {
            raw.push_back('\\'); advance();
            if (pos_ >= src_.size()) fatalf("bad char literal at line {}", sline);
            char esc = src_[pos_];
            raw.push_back(esc); advance();
            switch (esc) {
                case 'n': value = '\n'; break;
                case 't': value = '\t'; break;
                case 'r': value = '\r'; break;
                case '\\': value = '\\'; break;
                case '\'': value = '\''; break;
                case '"': value = '"'; break;
                case 'a': value = '\a'; break;
                case 'b': value = '\b'; break;
                case 'f': value = '\f'; break;
                case 'v': value = '\v'; break;
                case 'x': {
                    string hex;
                    while (pos_ < src_.size() && ch_isxdigit((unsigned char)src_[pos_])) {
                        raw.push_back(src_[pos_]); hex.push_back(src_[pos_]); advance();
                    }
                    value = (int)std::strtol(hex.c_str(), nullptr, 16);
                    break;
                }
                default:
                    if (esc >= '0' && esc <= '7') {
                        string oct(1, esc);
                        while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '7') {
                            raw.push_back(src_[pos_]); oct.push_back(src_[pos_]); advance();
                        }
                        value = (int)std::strtol(oct.c_str(), nullptr, 8);
                    } else value = (unsigned char)esc;
            }
        } else if (pos_ < src_.size()) {
            value = (unsigned char)src_[pos_];
            raw.push_back(src_[pos_]); advance();
        }
        if (pos_ >= src_.size() || src_[pos_] != '\'')
            fatalf("unterminated char literal at line {}", sline);
        advance();
        Token t{Tok::CharLit, raw, sline, scol};
        t.char_value = value;
        return t;
    }

    Token read_string_literal(int sline, int scol) {
        advance();
        string body;
        while (pos_ < src_.size() && src_[pos_] != '"') {
            if (src_[pos_] == '\\' && pos_ + 1 < src_.size()) {
                body.push_back(src_[pos_]); advance();
            }
            body.push_back(src_[pos_]); advance();
        }
        if (pos_ >= src_.size()) fatalf("unterminated string literal at line {}", sline);
        advance();
        return Token{Tok::StrLit, std::move(body), sline, scol};
    }

private:
    string src_;
    string file_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
};

// ============================================================================
// .y parser
// ============================================================================

class GrammarParser {
public:
    GrammarParser(string src, string filename, Grammar& g, Options& opts)
        : lex_(std::move(src), std::move(filename)), g_(g), opts_(opts) {
        g_.source_file = lex_.filename();
        // Order matters: reserve names so external codes 0/256/257 are anchored.
        g_.eof_sym   = g_.intern("$end", true);   g_.syms[g_.eof_sym].code   = 0;
        g_.error_sym = g_.intern("error", true);  g_.syms[g_.error_sym].code = 256;
        g_.undef_sym = g_.intern("$undefined", true); g_.syms[g_.undef_sym].code = 257;
        g_.syms[g_.eof_sym].display = "$end";
    }

    void parse() {
        peek_ = lex_.next_grammar();
        while (peek_.kind != Tok::EndOfFile && peek_.kind != Tok::DoublePercent)
            parse_declaration();
        if (peek_.kind == Tok::EndOfFile) fatal("missing %% before grammar rules");
        advance();

        while (peek_.kind != Tok::EndOfFile && peek_.kind != Tok::DoublePercent)
            parse_rule();

        if (peek_.kind == Tok::DoublePercent)
            g_.epilogue = lex_.take_to_end();
        finalize();
    }

private:
    void advance() { peek_ = lex_.next_grammar(); }
    bool at(Tok t) const { return peek_.kind == t; }
    void expect(Tok t, const char* what) {
        if (peek_.kind != t)
            fatalf("expected {} at line {}, got \"{}\"", what, peek_.line, peek_.text.c_str());
        advance();
    }

    int sym_of_char(const Token& t) {
        string nm = "'" + t.text + "'";
        int idx = g_.find(nm);
        if (idx < 0) {
            idx = g_.intern(nm, true);
            g_.syms[idx].code = t.char_value;
            g_.syms[idx].display = nm;
        }
        return idx;
    }

    int sym_of_strlit(const string& body) {
        string nm = "\"" + body + "\"";
        int idx = g_.find(nm);
        if (idx < 0) {
            idx = g_.intern(nm, true);
            g_.syms[idx].display = nm;
        }
        return idx;
    }

    void parse_declaration() {
        const Token t = peek_;
        switch (t.kind) {
            case Tok::PercentBraces: g_.prologue += t.text; advance(); return;
            case Tok::PercentToken: advance(); parse_token_decl(Assoc::None); return;
            case Tok::PercentLeft:  advance(); parse_token_decl(Assoc::Left); return;
            case Tok::PercentRight: advance(); parse_token_decl(Assoc::Right); return;
            case Tok::PercentNonassoc: advance(); parse_token_decl(Assoc::NonAssoc); return;
            case Tok::PercentPrecedence: advance(); parse_token_decl(Assoc::Precedence); return;
            case Tok::PercentType: advance(); parse_type_decl(); return;
            case Tok::PercentStart: advance(); parse_start_decl(); return;
            case Tok::PercentUnion: advance(); parse_union_decl(); return;
            case Tok::PercentExpect: advance(); parse_expect(false); return;
            case Tok::PercentExpectRR: advance(); parse_expect(true); return;
            case Tok::PercentDefine: advance(); parse_define(); return;
            case Tok::PercentCode: advance(); parse_code(); return;
            case Tok::PercentLocations: advance(); g_.want_locations = true; return;
            case Tok::PercentNamePrefix:
                advance();
                if (at(Tok::StrLit)) { g_.api_prefix = peek_.text; advance(); }
                return;
            case Tok::PercentLanguage:
            case Tok::PercentSkeleton:
            case Tok::PercentRequire:
            case Tok::PercentOutput:
            case Tok::PercentFilePrefix:
                advance();
                if (at(Tok::StrLit) || at(Tok::Identifier)) advance();
                return;
            case Tok::PercentDebug: advance(); return;
            case Tok::PercentDefines:
            case Tok::PercentHeader:
                advance();
                opts_.want_header = true;
                if (at(Tok::StrLit)) { opts_.defines_path = peek_.text; advance(); }
                return;
            case Tok::PercentVerbose: advance(); opts_.verbose = true; return;
            case Tok::PercentYacc: advance(); opts_.yacc_compat = true; return;
            case Tok::PercentPureParser:
            case Tok::PercentGlrParser:
                advance(); return;
            case Tok::PercentToken_Table: advance(); opts_.token_table = true; return;
            case Tok::PercentNoLines: advance(); opts_.no_lines = true; return;
            case Tok::PercentParseParam:
            case Tok::PercentLexParam:
            case Tok::PercentParam:
                advance();
                if (at(Tok::BraceBlock)) advance();
                return;
            case Tok::PercentDestructor:
            case Tok::PercentPrinter:
            case Tok::PercentInitialAction:
                advance();
                if (at(Tok::Tag)) advance();
                if (at(Tok::BraceBlock)) advance();
                while (at(Tok::Identifier) || at(Tok::CharLit) || at(Tok::StrLit)) advance();
                return;
            default:
                fatalf("unexpected '{}' at line {}", t.text.c_str(), t.line);
        }
    }

    void parse_define() {
        if (!at(Tok::Identifier)) {
            if (at(Tok::StrLit)) advance();
            return;
        }
        string name = peek_.text;
        advance();
        // Stitch hyphenated names: "lr.default-reduction" comes through as
        // "lr.default" Punct("-") Identifier("reduction").
        while (at(Tok::Punct) && peek_.text == "-") {
            name += "-";
            advance();
            if (at(Tok::Identifier)) { name += peek_.text; advance(); }
            else break;
        }
        if (at(Tok::StrLit) || at(Tok::Identifier) || at(Tok::Int)) {
            string v = peek_.text;
            if (name == "api.value.type") g_.api_value_type = v;
            else if (name == "api.prefix") g_.api_prefix = v;
            else if (name == "api.token.prefix") g_.token_prefix = v;
            advance();
        } else if (at(Tok::BraceBlock)) {
            string v = "{" + peek_.text + "}";
            if (name == "api.value.type") g_.api_value_type = v;
            advance();
        }
    }

    void parse_code() {
        string qual;
        if (at(Tok::Identifier)) { qual = peek_.text; advance(); }
        if (!at(Tok::BraceBlock)) fatalf("%code: expected {{ at line {}", peek_.line);
        const string& body = peek_.text;
        if (qual.empty()) g_.prologue += body;
        else if (qual == "requires") g_.prologue_requires += body;
        else if (qual == "provides") g_.prologue_provides += body;
        else if (qual == "top") g_.prologue_top += body;
        else g_.prologue += body;
        advance();
    }

    void parse_expect(bool rr) {
        if (!at(Tok::Int)) fatalf("expected integer at line {}", peek_.line);
        if (rr) g_.expected_rr = (int)peek_.ival; else g_.expected_sr = (int)peek_.ival;
        advance();
    }

    void parse_union_decl() {
        if (at(Tok::Identifier)) { g_.api_value_union_name = peek_.text; advance(); }
        if (!at(Tok::BraceBlock)) fatalf("%union: expected {{ at line {}", peek_.line);
        g_.union_body = peek_.text;
        g_.has_union = true;
        advance();
    }

    void parse_start_decl() {
        if (!at(Tok::Identifier))
            fatalf("expected identifier after %start at line {}", peek_.line);
        int idx = g_.find(peek_.text);
        if (idx < 0) idx = g_.intern(peek_.text, false);
        else g_.syms[idx].is_terminal = false;
        g_.start_sym = idx;
        advance();
    }

    void parse_type_decl() {
        string tag;
        if (at(Tok::Tag)) { tag = peek_.text; advance(); }
        while (true) {
            int idx = -1;
            if (at(Tok::Identifier)) {
                idx = g_.find(peek_.text);
                if (idx < 0) idx = g_.intern(peek_.text, false);
                advance();
            } else if (at(Tok::CharLit)) {
                idx = sym_of_char(peek_); advance();
            } else break;
            if (!tag.empty()) g_.syms[idx].type_tag = tag;
        }
    }

    int next_prec_ = 0;
    void parse_token_decl(Assoc assoc) {
        string tag;
        if (at(Tok::Tag)) { tag = peek_.text; advance(); }
        int prec_level = 0;
        if (assoc != Assoc::None) prec_level = ++next_prec_;
        bool any = false;
        while (true) {
            int idx = -1;
            int explicit_code = -1;
            string alias_str;
            if (at(Tok::Identifier)) {
                idx = g_.find(peek_.text);
                if (idx < 0) idx = g_.intern(peek_.text, true);
                else g_.syms[idx].is_terminal = true;
                advance();
                if (at(Tok::Int)) {
                    if (peek_.ival < 0 || peek_.ival > 65535)
                        fatalf("token code out of range (0..65535) at line {}", peek_.line);
                    explicit_code = (int)peek_.ival;
                    advance();
                }
                if (at(Tok::StrLit)) { alias_str = peek_.text; advance(); }
            } else if (at(Tok::CharLit)) {
                idx = sym_of_char(peek_); advance();
                if (at(Tok::StrLit)) advance();
            } else if (at(Tok::StrLit)) {
                idx = sym_of_strlit(peek_.text); advance();
            } else break;
            any = true;
            if (!tag.empty()) g_.syms[idx].type_tag = tag;
            if (explicit_code >= 0) g_.syms[idx].code = explicit_code;
            if (assoc != Assoc::None) {
                g_.syms[idx].assoc = assoc;
                g_.syms[idx].prec = prec_level;
            }
            if (!alias_str.empty()) {
                int aidx = sym_of_strlit(alias_str);
                g_.syms[aidx].alias_of = idx;
                g_.syms[aidx].is_terminal = true;
            }
        }
        if (!any) fatalf("no symbols in declaration at line {}", peek_.line);
    }

    void parse_rule() {
        if (peek_.kind != Tok::Identifier_Colon)
            fatalf("expected rule LHS at line {}, got \"{}\"", peek_.line, peek_.text.c_str());
        Token lhs = peek_;
        advance();
        string lhs_name;
        if (at(Tok::BracketName)) { lhs_name = peek_.text; advance(); }
        expect(Tok::Colon, "':'");
        int lhs_idx = g_.find(lhs.text);
        if (lhs_idx < 0) lhs_idx = g_.intern(lhs.text, false);
        else g_.syms[lhs_idx].is_terminal = false;
        g_.syms[lhs_idx].defined = true;
        if (g_.start_sym < 0) g_.start_sym = lhs_idx;

        parse_alternative(lhs_idx, lhs_name);
        while (at(Tok::Or)) { advance(); parse_alternative(lhs_idx, lhs_name); }
        if (at(Tok::Semi)) advance();
    }

    void parse_alternative(int lhs_idx, const string& lhs_name) {
        Production p;
        p.lhs = lhs_idx;
        p.line = peek_.line;
        p.lhs_name = lhs_name;
        p.lhs_tag = g_.syms[lhs_idx].type_tag;
        bool had_prec = false;
        Token last_brace{};
        bool has_action = false;

        while (true) {
            if (at(Tok::Identifier)) {
                Token t = peek_; advance();
                int idx = g_.find(t.text);
                if (idx < 0) idx = g_.intern(t.text, false);
                g_.syms[idx].used = true;
                p.rhs.push_back(idx);
                p.rhs_tags.push_back(g_.syms[idx].type_tag);
                string a;
                if (at(Tok::BracketName)) { a = peek_.text; advance(); }
                p.rhs_names.push_back(a);
            } else if (at(Tok::CharLit)) {
                int idx = sym_of_char(peek_); advance();
                g_.syms[idx].used = true;
                p.rhs.push_back(idx);
                p.rhs_tags.push_back(g_.syms[idx].type_tag);
                string a;
                if (at(Tok::BracketName)) { a = peek_.text; advance(); }
                p.rhs_names.push_back(a);
            } else if (at(Tok::StrLit)) {
                int idx = sym_of_strlit(peek_.text); advance();
                g_.syms[idx].is_terminal = true;
                g_.syms[idx].used = true;
                p.rhs.push_back(idx);
                p.rhs_tags.push_back(g_.syms[idx].type_tag);
                string a;
                if (at(Tok::BracketName)) { a = peek_.text; advance(); }
                p.rhs_names.push_back(a);
            } else if (at(Tok::PercentEmpty)) {
                advance();
            } else if (at(Tok::PercentPrec)) {
                advance(); had_prec = true;
                int idx = -1;
                if (at(Tok::Identifier)) {
                    idx = g_.find(peek_.text);
                    if (idx < 0) idx = g_.intern(peek_.text, true);
                    advance();
                } else if (at(Tok::CharLit)) { idx = sym_of_char(peek_); advance(); }
                else fatalf("expected token after %prec at line {}", peek_.line);
                p.prec = g_.syms[idx].prec;
                p.assoc = g_.syms[idx].assoc;
                p.prec_sym = idx;
            } else if (at(Tok::PercentDPrec)) {
                advance();
                if (at(Tok::Int)) advance();
            } else if (at(Tok::PercentMerge)) {
                advance();
                if (at(Tok::Tag)) advance();
            } else if (at(Tok::BraceBlock)) {
                Token b = peek_;
                advance();
                if (at(Tok::Or) || at(Tok::Semi) ||
                    at(Tok::EndOfFile) || peek_.kind == Tok::Identifier_Colon) {
                    p.action = b.text;
                    p.action_line = (int)b.ival;
                    has_action = true;
                    break;
                } else {
                    // Mid-rule action: synthesize $@N -> /* empty */ { action } ;
                    static int mr_id = 0;
                    std::string mr_name = "$@" + std::to_string(++mr_id);
                    int mr_idx = g_.intern(mr_name, false);
                    g_.syms[mr_idx].defined = true;
                    Production mp;
                    mp.lhs = mr_idx;
                    mp.action = b.text;
                    mp.action_line = (int)b.ival;
                    mp.line = b.line;
                    mp.midrule_offset = (int)p.rhs.size();
                    // Snapshot parent's tags & names so $k references inside
                    // the mid-rule action can find the tag of the k-th parent symbol.
                    mp.rhs_tags = p.rhs_tags;
                    mp.rhs_names = p.rhs_names;
                    g_.prods.push_back(std::move(mp));
                    p.rhs.push_back(mr_idx);
                    p.rhs_tags.push_back("");
                    p.rhs_names.push_back("");
                }
            } else break;
        }
        (void)last_brace; (void)has_action;
        if (!had_prec) {
            for (auto it = p.rhs.rbegin(); it != p.rhs.rend(); ++it) {
                if (g_.syms[*it].is_terminal && g_.syms[*it].prec > 0) {
                    p.prec = g_.syms[*it].prec;
                    p.assoc = g_.syms[*it].assoc;
                    p.prec_sym = *it;
                    break;
                }
            }
        }
        g_.prods.push_back(std::move(p));
    }

    void finalize() {
        // Resolve string-literal aliases: replace alias index uses with canonical.
        // For each production rhs slot, if symbol is alias_of >= 0, replace.
        for (auto& p : g_.prods) {
            for (auto& s : p.rhs) {
                if (g_.syms[s].alias_of >= 0) s = g_.syms[s].alias_of;
            }
        }
        if (g_.start_sym >= 0 && g_.syms[g_.start_sym].alias_of >= 0)
            g_.start_sym = g_.syms[g_.start_sym].alias_of;
        for (auto& s : g_.syms) {
            if (s.name.empty()) continue;
            if (!s.defined && s.alias_of < 0 &&
                s.name != "error" && s.name != "$end" && s.name != "$undefined" &&
                s.name[0] != '$') {
                s.is_terminal = true;
            }
        }
        if (g_.start_sym < 0) fatal("no rules defined");
    }

    Lexer lex_;
    Grammar& g_;
    Options& opts_;
    Token peek_;
};

// ============================================================================
// LALR(1) construction
// ============================================================================

struct Item { uint32_t prod; uint32_t dot; };
inline bool operator<(const Item& a, const Item& b) {
    return std::tie(a.prod, a.dot) < std::tie(b.prod, b.dot);
}
inline bool operator==(const Item& a, const Item& b) {
    return a.prod == b.prod && a.dot == b.dot;
}

struct State {
    vector<Item> kernel;
    vector<Item> items;
    std::map<int, int> trans;
    vector<std::set<int>> la;     // per-kernel-item: lookahead set in INTERNAL terminal indices
};

class LALR {
public:
    static constexpr int ACCEPT = std::numeric_limits<int>::max();

    LALR(Grammar& g) : g_(g) {}

    void build() {
        augment_and_number();
        compute_nullable();
        compute_first();
        build_lr0();
        compute_lookaheads();
        build_action_goto();
    }

    int n_terminals()    const { return (int)term_internal_.size(); }
    int n_nonterminals() const { return (int)nonterm_internal_.size(); }
    int n_total_syms()   const { return n_terminals() + n_nonterminals(); }
    int n_states()       const { return (int)states_.size(); }
    int n_rules()        const { return (int)g_.prods.size(); }

    int sym_to_internal(int s) const { return sym_to_internal_[s]; }
    int internal_to_sym(int i) const { return internal_to_sym_[i]; }
    int term_external_code(int internal) const { return term_codes_[internal]; }
    int term_external_max() const { return term_max_external_; }
    const vector<int>& translate_table() const { return translate_; }
    int eof_internal() const { return 0; }
    int error_internal() const { return error_internal_; }
    int undef_internal() const { return undef_internal_; }
    int augmented_rule() const { return 0; }
    int final_state() const { return final_state_; }

    int action(int s, int t) const { return action_[s * n_terminals() + t]; }
    int goto_tab(int s, int nt) const { return goto_[s * n_nonterminals() + nt]; }

    int prod_lhs_internal(int i) const { return sym_to_internal_[g_.prods[i].lhs]; }
    const Production& prod(int i) const { return g_.prods[i]; }

    int sr_conflicts() const { return sr_conflicts_; }
    int rr_conflicts() const { return rr_conflicts_; }

    vector<int> default_reductions() const { return default_reduce_; }
    vector<int> stos_internal() const {
        vector<int> v(n_states());
        for (int s = 0; s < n_states(); s++) {
            int sym = state_access_[s];
            v[s] = (sym < 0) ? 0 : sym_to_internal_[sym];
        }
        return v;
    }
    vector<int> rule_lengths() const {
        vector<int> v;
        for (auto& p : g_.prods) v.push_back((int)p.rhs.size());
        return v;
    }
    vector<int> rule_lines() const {
        vector<int> v;
        for (auto& p : g_.prods) v.push_back(p.line);
        return v;
    }

private:
    Grammar& g_;
    vector<bool> nullable_;
    vector<std::set<int>> sym_first_;
    vector<State> states_;
    int final_state_ = -1;

    vector<int> term_internal_;
    vector<int> nonterm_internal_;
    vector<int> sym_to_internal_;
    vector<int> internal_to_sym_;
    vector<int> term_codes_;
    vector<int> translate_;
    int term_max_external_ = 0;
    int error_internal_ = -1;
    int undef_internal_ = -1;

    vector<int> action_;
    vector<int> goto_;
    vector<int> default_reduce_;
    vector<int> state_access_;

    int sr_conflicts_ = 0;
    int rr_conflicts_ = 0;

    void augment_and_number() {
        // $accept: start $end
        int accept = g_.intern("$accept", false);
        g_.accept_sym = accept;
        g_.syms[accept].defined = true;
        Production p;
        p.lhs = accept;
        p.rhs.push_back(g_.start_sym);
        p.rhs.push_back(g_.eof_sym);
        p.rhs_tags = {"", ""};
        p.rhs_names = {"", ""};
        p.line = 0;
        g_.prods.insert(g_.prods.begin(), std::move(p));

        // Internal numbering: terminals first.
        // Order: $end (0), error (1), $undefined (2), then user terminals in declaration order.
        vector<int> terms = {g_.eof_sym, g_.error_sym, g_.undef_sym};
        for (int i = 0; i < (int)g_.syms.size(); i++) {
            if (i == g_.eof_sym || i == g_.error_sym || i == g_.undef_sym) continue;
            if (g_.syms[i].is_terminal && g_.syms[i].alias_of < 0) terms.push_back(i);
        }
        vector<int> nonterms = {accept};
        for (int i = 0; i < (int)g_.syms.size(); i++) {
            if (i == accept) continue;
            if (!g_.syms[i].is_terminal) nonterms.push_back(i);
        }
        sym_to_internal_.assign(g_.syms.size(), -1);
        internal_to_sym_.clear();
        term_internal_.clear();
        nonterm_internal_.clear();
        for (int i = 0; i < (int)terms.size(); i++) {
            sym_to_internal_[terms[i]] = i;
            internal_to_sym_.push_back(terms[i]);
            term_internal_.push_back(terms[i]);
        }
        for (int i = 0; i < (int)nonterms.size(); i++) {
            int idx = (int)terms.size() + i;
            sym_to_internal_[nonterms[i]] = idx;
            internal_to_sym_.push_back(nonterms[i]);
            nonterm_internal_.push_back(nonterms[i]);
        }
        // String-literal aliases were resolved at parse-finalize; they share alias_of's internal.
        for (int i = 0; i < (int)g_.syms.size(); i++) {
            if (g_.syms[i].alias_of >= 0) sym_to_internal_[i] = sym_to_internal_[g_.syms[i].alias_of];
        }
        error_internal_ = sym_to_internal_[g_.error_sym];
        undef_internal_ = sym_to_internal_[g_.undef_sym];

        // Assign external codes to user-named tokens that don't have one.
        int next_code = 258;
        // First scan to detect explicit code maxima.
        for (int s : term_internal_) if (g_.syms[s].code >= next_code) next_code = g_.syms[s].code + 1;
        for (int s : term_internal_) {
            if (g_.syms[s].code < 0) g_.syms[s].code = next_code++;
        }
        // Build translate[]
        term_max_external_ = 0;
        for (int s : term_internal_) term_max_external_ = std::max(term_max_external_, g_.syms[s].code);
        if (term_max_external_ < 0 || term_max_external_ > 65535)
            fatalf("token code out of supported range (max=65535)");
        translate_.assign(term_max_external_ + 1, undef_internal_);
        // Always map 0 (EOF) to $end's internal.
        if (!term_internal_.empty()) translate_[0] = sym_to_internal_[g_.eof_sym];
        for (int s : term_internal_) {
            if (g_.syms[s].code >= 0 && g_.syms[s].code <= term_max_external_)
                translate_[g_.syms[s].code] = sym_to_internal_[s];
        }
        term_codes_.assign(term_internal_.size(), 0);
        for (int i = 0; i < (int)term_internal_.size(); i++)
            term_codes_[i] = g_.syms[term_internal_[i]].code;
    }

    void compute_nullable() {
        nullable_.assign(g_.syms.size(), false);
        bool ch = true;
        while (ch) {
            ch = false;
            for (auto& p : g_.prods) {
                if (nullable_[p.lhs]) continue;
                bool all = true;
                for (int s : p.rhs) {
                    if (g_.syms[s].is_terminal) { all = false; break; }
                    if (!nullable_[s]) { all = false; break; }
                }
                if (all) { nullable_[p.lhs] = true; ch = true; }
            }
        }
        for (int i = 0; i < (int)g_.syms.size(); i++)
            g_.syms[i].is_nullable = nullable_[i];
    }

    void compute_first() {
        sym_first_.assign(g_.syms.size(), {});
        for (int i = 0; i < (int)g_.syms.size(); i++)
            if (g_.syms[i].is_terminal) sym_first_[i].insert(sym_to_internal_[i]);
        bool ch = true;
        while (ch) {
            ch = false;
            for (auto& p : g_.prods) {
                size_t before = sym_first_[p.lhs].size();
                for (int s : p.rhs) {
                    for (int t : sym_first_[s]) sym_first_[p.lhs].insert(t);
                    if (!nullable_[s]) break;
                }
                if (sym_first_[p.lhs].size() != before) ch = true;
            }
        }
    }

    void first_of_seq(const vector<int>& seq, size_t start, std::set<int>& out, bool& nullable_seq) {
        nullable_seq = true;
        for (size_t k = start; k < seq.size(); k++) {
            int s = seq[k];
            for (int t : sym_first_[s]) out.insert(t);
            if (!nullable_[s]) { nullable_seq = false; return; }
        }
    }

    vector<vector<int>> by_lhs_;
    void index_by_lhs() {
        by_lhs_.assign(g_.syms.size(), {});
        for (int i = 0; i < (int)g_.prods.size(); i++) by_lhs_[g_.prods[i].lhs].push_back(i);
    }

    vector<Item> closure(const vector<Item>& kernel) {
        std::set<Item> cur(kernel.begin(), kernel.end());
        vector<Item> work(kernel.begin(), kernel.end());
        size_t i = 0;
        while (i < work.size()) {
            Item it = work[i++];
            const auto& rhs = g_.prods[it.prod].rhs;
            if (it.dot >= rhs.size()) continue;
            int X = rhs[it.dot];
            if (g_.syms[X].is_terminal) continue;
            for (int p : by_lhs_[X]) {
                Item ni{(uint32_t)p, 0};
                if (cur.insert(ni).second) work.push_back(ni);
            }
        }
        vector<Item> out(cur.begin(), cur.end());
        std::sort(out.begin(), out.end());
        return out;
    }

    void build_lr0() {
        index_by_lhs();
        Item start{0u, 0u};
        State s0;
        s0.kernel = {start};
        s0.items = closure(s0.kernel);
        s0.la.assign(1, {});
        states_.push_back(std::move(s0));
        state_access_.push_back(-1);

        std::map<vector<Item>, int> by_kernel;
        by_kernel[states_[0].kernel] = 0;

        for (int i = 0; i < (int)states_.size(); i++) {
            std::map<int, vector<Item>> next;
            for (auto& it : states_[i].items) {
                const auto& rhs = g_.prods[it.prod].rhs;
                if (it.dot >= rhs.size()) continue;
                int X = rhs[it.dot];
                next[X].push_back(Item{it.prod, it.dot + 1});
            }
            for (auto& [X, kernel] : next) {
                std::sort(kernel.begin(), kernel.end());
                kernel.erase(std::unique(kernel.begin(), kernel.end()), kernel.end());
                int target;
                auto it = by_kernel.find(kernel);
                if (it == by_kernel.end()) {
                    target = (int)states_.size();
                    State st;
                    st.kernel = kernel;
                    st.items = closure(kernel);
                    st.la.assign(kernel.size(), {});
                    states_.push_back(std::move(st));
                    state_access_.push_back(X);
                    by_kernel[states_.back().kernel] = target;
                } else target = it->second;
                states_[i].trans[X] = target;
            }
        }

        // YYFINAL: state where dot is past full augmented rhs.
        size_t aug_size = g_.prods[0].rhs.size();
        for (int i = 0; i < (int)states_.size(); i++)
            for (auto& it : states_[i].kernel)
                if (it.prod == 0 && it.dot == aug_size) final_state_ = i;
    }

    void compute_lookaheads() {
        // Initial: state 0 kernel item ($accept -> . S $end) gets {$end}.
        states_[0].la[0].insert(0);

        struct Edge { int from_s; int from_i; int to_s; int to_i; };
        vector<Edge> propagate;

        const int DUMMY = -1;

        for (int s = 0; s < (int)states_.size(); s++) {
            for (int ki = 0; ki < (int)states_[s].kernel.size(); ki++) {
                Item kit = states_[s].kernel[ki];
                // LR(1) closure of {[kit, #]}
                struct LRI { Item core; int la; };
                struct C {
                    bool operator()(const LRI& a, const LRI& b) const {
                        return std::tie(a.core.prod, a.core.dot, a.la)
                             < std::tie(b.core.prod, b.core.dot, b.la);
                    }
                };
                std::set<LRI, C> J;
                J.insert({kit, DUMMY});
                vector<LRI> work(J.begin(), J.end());
                for (size_t qi = 0; qi < work.size(); qi++) {
                    LRI li = work[qi];
                    const auto& rhs = g_.prods[li.core.prod].rhs;
                    if (li.core.dot >= rhs.size()) continue;
                    int X = rhs[li.core.dot];
                    if (g_.syms[X].is_terminal) continue;
                    std::set<int> firstset;
                    bool nullable_seq = true;
                    first_of_seq(rhs, li.core.dot + 1, firstset, nullable_seq);
                    if (nullable_seq) firstset.insert(li.la);
                    for (int p : by_lhs_[X]) {
                        for (int t : firstset) {
                            LRI ni{Item{(uint32_t)p, 0}, t};
                            if (J.insert(ni).second) work.push_back(ni);
                        }
                    }
                }
                // Distribute to successors
                for (auto& li : J) {
                    const auto& rhs = g_.prods[li.core.prod].rhs;
                    if (li.core.dot >= rhs.size()) continue;
                    int X = rhs[li.core.dot];
                    auto tit = states_[s].trans.find(X);
                    if (tit == states_[s].trans.end()) continue;
                    int dst = tit->second;
                    Item core_after{li.core.prod, li.core.dot + 1};
                    int dst_idx = -1;
                    auto& dk = states_[dst].kernel;
                    for (int j = 0; j < (int)dk.size(); j++)
                        if (dk[j] == core_after) { dst_idx = j; break; }
                    if (dst_idx < 0) continue;
                    if (li.la == DUMMY) propagate.push_back(Edge{s, ki, dst, dst_idx});
                    else states_[dst].la[dst_idx].insert(li.la);
                }
            }
        }
        bool ch = true;
        while (ch) {
            ch = false;
            for (auto& e : propagate) {
                size_t before = states_[e.to_s].la[e.to_i].size();
                for (int t : states_[e.from_s].la[e.from_i])
                    states_[e.to_s].la[e.to_i].insert(t);
                if (states_[e.to_s].la[e.to_i].size() != before) ch = true;
            }
        }
    }

    // Build extended item sets: LR(1) closure of (kernel item, kernel-LA) per state.
    // Returns list of {prod, dot, lookahead-set} per state.
    struct ExtItem { Item core; std::set<int> la; };
    vector<vector<ExtItem>> compute_ext_items() {
        vector<vector<ExtItem>> R(states_.size());
        for (int s = 0; s < (int)states_.size(); s++) {
            // For each kernel item with its lookahead set, do LR(1) closure.
            std::map<Item, std::set<int>> agg;
            for (int ki = 0; ki < (int)states_[s].kernel.size(); ki++) {
                Item kit = states_[s].kernel[ki];
                // BFS-like propagation
                std::map<Item, std::set<int>> work;
                work[kit] = states_[s].la[ki];
                bool changed = true;
                while (changed) {
                    changed = false;
                    auto snapshot = work;
                    for (auto& [it, las] : snapshot) {
                        const auto& rhs = g_.prods[it.prod].rhs;
                        if (it.dot >= rhs.size()) continue;
                        int X = rhs[it.dot];
                        if (g_.syms[X].is_terminal) continue;
                        // Compute FIRST(beta * la) where beta = rhs after dot+1
                        std::set<int> firstset;
                        bool nullable_seq = true;
                        first_of_seq(rhs, it.dot + 1, firstset, nullable_seq);
                        if (nullable_seq) for (int t : las) firstset.insert(t);
                        for (int p : by_lhs_[X]) {
                            Item ni{(uint32_t)p, 0};
                            auto& dst = work[ni];
                            size_t before = dst.size();
                            for (int t : firstset) dst.insert(t);
                            if (dst.size() != before) changed = true;
                        }
                    }
                }
                for (auto& [it, las] : work) {
                    auto& d = agg[it];
                    for (int t : las) d.insert(t);
                }
            }
            R[s].reserve(agg.size());
            for (auto& [it, las] : agg) R[s].push_back({it, std::move(las)});
        }
        return R;
    }

    void build_action_goto() {
        int nT = n_terminals();
        int nN = n_nonterminals();
        int nS = n_states();
        action_.assign((size_t)nS * nT, 0);
        goto_.assign((size_t)nS * nN, 0);
        default_reduce_.assign(nS, 0);

        // Shifts and gotos
        for (int s = 0; s < nS; s++) {
            for (auto& [X, dst] : states_[s].trans) {
                int internal = sym_to_internal_[X];
                if (g_.syms[X].is_terminal) {
                    action_[s * nT + internal] = dst + 1;
                } else {
                    goto_[s * nN + (internal - nT)] = dst + 1;
                }
            }
        }
        // Reduces: use extended item closure to find all final items per state.
        auto ext = compute_ext_items();
        for (int s = 0; s < nS; s++) {
            for (auto& ei : ext[s]) {
                const auto& rhs = g_.prods[ei.core.prod].rhs;
                if (ei.core.dot < rhs.size()) continue;
                if (ei.core.prod == 0) {
                    action_[s * nT + 0] = ACCEPT;
                    continue;
                }
                for (int la : ei.la) {
                    int idx = s * nT + la;
                    int cur = action_[idx];
                    int red = -((int)ei.core.prod);
                    if (cur == 0) action_[idx] = red;
                    else if (cur == ACCEPT) {
                        // accept stays
                    } else if (cur > 0) {
                        // shift/reduce
                        int term_sym = internal_to_sym_[la];
                        int rule_prec = g_.prods[ei.core.prod].prec;
                        Assoc rule_assoc = g_.prods[ei.core.prod].assoc;
                        int tok_prec = g_.syms[term_sym].prec;
                        Assoc tok_assoc = g_.syms[term_sym].assoc;
                        if (rule_prec > 0 && tok_prec > 0) {
                            if (tok_prec > rule_prec) {
                                // shift wins
                            } else if (tok_prec < rule_prec) {
                                action_[idx] = red;
                            } else {
                                // equal precedence
                                if (rule_assoc == Assoc::Left) action_[idx] = red;
                                else if (rule_assoc == Assoc::Right) {
                                    // shift wins
                                } else if (rule_assoc == Assoc::NonAssoc ||
                                           rule_assoc == Assoc::Precedence) {
                                    // mark as error
                                    action_[idx] = ERR_MARK;
                                }
                            }
                            (void)tok_assoc;
                        } else {
                            sr_conflicts_++;
                            // default: prefer shift
                        }
                    } else {
                        // reduce/reduce
                        int existing_rule = -cur;
                        rr_conflicts_++;
                        if ((int)ei.core.prod < existing_rule) action_[idx] = red;
                    }
                }
            }
        }
        // Default reductions (only for states where the only non-error action is one reduce).
        for (int s = 0; s < nS; s++) {
            int last = 0; int distinct = 0; int reduces = 0; bool shift = false;
            for (int t = 0; t < nT; t++) {
                int a = action_[s * nT + t];
                if (a == 0 || a == ERR_MARK) continue;
                if (a == ACCEPT) { /* accept doesn't block default */ continue; }
                if (a > 0) shift = true;
                else if (a < 0) {
                    if (last == 0) { last = a; distinct = 1; }
                    else if (last != a) distinct++;
                    reduces++;
                }
            }
            if (!shift && reduces > 0 && distinct == 1) {
                default_reduce_[s] = -last;
                for (int t = 0; t < nT; t++) {
                    if (action_[s * nT + t] == last) action_[s * nT + t] = 0;
                }
            }
        }
    }

public:
    static constexpr int ERR_MARK = -1000000;
};

// ============================================================================
// Code emission
// ============================================================================

class Emitter {
public:
    Emitter(const Grammar& g, const LALR& l, const Options& o)
        : g_(g), l_(l), opts_(o) {}

    void emit(Buf out, Buf* hdr) {
        emit_prefix(out);
        if (hdr) emit_prefix(*hdr);

        // Tokens & YYSTYPE: emitted in both header (if any) and source.
        std::string tokens_s, vt_s;
        Buf tokens{tokens_s};
        Buf value_type{vt_s};
        emit_token_kinds(tokens);
        emit_value_type(value_type);

        if (hdr) {
            *hdr << "#ifndef YY_TAB_H_INCLUDED\n# define YY_TAB_H_INCLUDED\n";
            *hdr << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n";
            *hdr << "#ifndef YYDEBUG\n# define YYDEBUG 0\n#endif\n";
            *hdr << "#if YYDEBUG\nextern int yydebug;\n#endif\n";
            if (!g_.prologue_requires.empty())
                *hdr << "/* %code requires */\n" << g_.prologue_requires << "\n";
            *hdr << tokens_s;
            *hdr << vt_s;
            *hdr << "extern YYSTYPE yylval;\n";
            if (g_.want_locations) *hdr << "extern YYLTYPE yylloc;\n";
            *hdr << "int yyparse(void);\n";
            if (!g_.prologue_provides.empty())
                *hdr << "/* %code provides */\n" << g_.prologue_provides << "\n";
            *hdr << "#ifdef __cplusplus\n}\n#endif\n";
            *hdr << "#endif\n";
        }

        // Source
        if (!g_.prologue_top.empty()) out << g_.prologue_top << "\n";
        out << "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n";
        if (!g_.prologue_requires.empty()) out << g_.prologue_requires << "\n";
        if (!g_.prologue.empty()) {
            if (!opts_.no_lines) out << "#line 1 \"" << g_.source_file << "\"\n";
            out << g_.prologue << "\n";
        }
        out << tokens_s;
        out << vt_s;

        emit_constants(out);
        emit_translation_table(out);
        emit_compressed_tables(out);
        emit_misc_tables(out);
        emit_yyerror_default(out);
        emit_driver(out);
        emit_action_switch(out);
        emit_driver_tail(out);

        if (!g_.epilogue.empty()) {
            if (!opts_.no_lines) out << "\n#line " << /*approx*/ 1 << " \"" << g_.source_file << "\"\n";
            out << g_.epilogue;
        }
    }

private:
    const Grammar& g_;
    const LALR& l_;
    const Options& opts_;

    void emit_prefix(Buf out) {
        out << "/* yacc.cpp generated parser */\n";
    }

    static string esc(const string& s) {
        std::string r;
        for (char c : s) {
            switch (c) {
                case '\\': r += "\\\\"; break;
                case '"': r += "\\\""; break;
                case '\n': r += "\\n"; break;
                case '\t': r += "\\t"; break;
                case '\r': r += "\\r"; break;
                default:
                    if ((unsigned char)c < 0x20) {
                        unsigned char uc = (unsigned char)c;
                        r += '\\';
                        r += (char)('0' + ((uc >> 6) & 7));
                        r += (char)('0' + ((uc >> 3) & 7));
                        r += (char)('0' + (uc & 7));
                    } else r += c;
            }
        }
        return r;
    }

    void emit_token_kinds(Buf out) {
        out << "#ifndef YYTOKENTYPE\n# define YYTOKENTYPE\n";
        out << "  enum yytokentype {\n";
        out << "    YYEMPTY = -2,\n";
        out << "    YYEOF = 0,\n";
        out << "    YYerror = 256,\n";
        out << "    YYUNDEF = 257";
        for (int s = 0; s < (int)g_.syms.size(); s++) {
            const Symbol& sm = g_.syms[s];
            if (!sm.is_terminal) continue;
            if (s == g_.eof_sym || s == g_.error_sym || s == g_.undef_sym) continue;
            if (sm.alias_of >= 0) continue;
            if (sm.name.empty()) continue;
            if (sm.name[0] == '\'' || sm.name[0] == '"' || sm.name[0] == '$') continue;
            out << ",\n    " << sm.name << " = " << sm.code;
        }
        out << "\n  };\n";
        out << "  typedef enum yytokentype yytoken_kind_t;\n";
        out << "#endif\n";
        // #defines (idempotent)
        for (int s = 0; s < (int)g_.syms.size(); s++) {
            const Symbol& sm = g_.syms[s];
            if (!sm.is_terminal || sm.alias_of >= 0) continue;
            if (s == g_.eof_sym || s == g_.error_sym || s == g_.undef_sym) continue;
            if (sm.name.empty()) continue;
            if (sm.name[0] == '\'' || sm.name[0] == '"' || sm.name[0] == '$') continue;
            out << "#ifndef " << sm.name << "\n# define " << sm.name << " " << sm.code << "\n#endif\n";
        }
    }

    void emit_value_type(Buf out) {
        out << "#if !defined YYSTYPE && !defined YYSTYPE_IS_DECLARED\n";
        if (g_.has_union) {
            if (!opts_.no_lines)
                out << "#line 1 \"" << g_.source_file << "\"\n";
            out << "typedef union";
            if (!g_.api_value_union_name.empty()) out << " " << g_.api_value_union_name;
            out << " {\n" << g_.union_body << "\n} YYSTYPE;\n";
        } else if (g_.api_value_type == "union") {
            std::set<string> tags;
            for (auto& s : g_.syms) if (!s.type_tag.empty()) tags.insert(s.type_tag);
            out << "typedef union {\n";
            int i = 0;
            for (auto& t : tags) out << "  " << t << " yyu" << i++ << ";\n";
            out << "} YYSTYPE;\n";
        } else {
            out << "typedef int YYSTYPE;\n";
        }
        out << "# define YYSTYPE_IS_TRIVIAL 1\n";
        out << "# define YYSTYPE_IS_DECLARED 1\n";
        out << "#endif\n";
        if (g_.want_locations) {
            out << "#if !defined YYLTYPE && !defined YYLTYPE_IS_DECLARED\n";
            out << "typedef struct YYLTYPE {\n";
            out << "  int first_line; int first_column;\n";
            out << "  int last_line;  int last_column;\n";
            out << "} YYLTYPE;\n";
            out << "# define YYLTYPE_IS_TRIVIAL 1\n";
            out << "# define YYLTYPE_IS_DECLARED 1\n";
            out << "#endif\n";
        }
    }

    void emit_constants(Buf out) {
        out << "YYSTYPE yylval;\nint yychar;\nint yynerrs;\n";
        if (g_.want_locations)
            out << "YYLTYPE yylloc;\n";
        out << "#ifndef YYDEBUG\n# define YYDEBUG 0\n#endif\n";
        out << "#if YYDEBUG\nint yydebug;\n#endif\n";
        out << "#define YYNTOKENS " << l_.n_terminals() << "\n";
        out << "#define YYNNTS " << l_.n_nonterminals() << "\n";
        out << "#define YYNRULES " << l_.n_rules() << "\n";
        out << "#define YYNSTATES " << l_.n_states() << "\n";
        out << "#define YYMAXUTOK " << l_.term_external_max() << "\n";
        out << "#define YYINITDEPTH 200\n#define YYMAXDEPTH 10000\n";
        out << "#define YYACCEPT goto yyacceptlab\n";
        out << "#define YYABORT goto yyabortlab\n";
        out << "#define YYERROR goto yyerrorlab\n";
        out << "#define YYRECOVERING() (!!yyerrstatus)\n";
        out << "#define yyerrok (yyerrstatus = 0)\n";
        out << "#define yyclearin (yychar = -2)\n";
        out << "#define YYFINAL " << l_.final_state() << "\n";
    }

    template <class T>
    void emit_array(Buf out, const char* type, const char* name, const std::vector<T>& v) {
        out << "static const " << type << " " << name << "[] = {\n";
        if (v.empty()) out << "  0\n};\n";
        else {
            for (size_t i = 0; i < v.size(); i++) {
                if (i % 16 == 0) out << "  ";
                out << (long long)v[i];
                if (i + 1 != v.size()) out << ", ";
                if ((i + 1) % 16 == 0 && i + 1 != v.size()) out << "\n";
            }
            out << "\n};\n";
        }
    }

    void emit_translation_table(Buf out) {
        emit_array(out, "short", "yytranslate", l_.translate_table());
    }

    void emit_compressed_tables(Buf out) {
        // Layout: per-state row of nT entries appended to yytable/yycheck.
        // yypact[s] = base offset OR NINF (default-only).
        // For state s and token t: yytable[base+t] holds action; yycheck[base+t] = t IFF
        // state s has a real action for token t, else yycheck = -1 so the
        //   "yycheck[idx] != yytoken" check filters out the hole.
        // Gotos work the same way over states.
        const int NINF = -32768;
        int nT = l_.n_terminals();
        int nN = l_.n_nonterminals();
        int nS = l_.n_states();
        vector<int> yypact(nS, NINF), yypgoto(nN, NINF), yydefgoto(nN, 0);
        vector<int> yytable, yycheck;
        for (int s = 0; s < nS; s++) {
            // Determine whether state has any action; if not, leave yypact[s] = NINF.
            bool has_any = false;
            for (int t = 0; t < nT; t++) {
                int a = l_.action(s, t);
                if (a != 0) { has_any = true; break; }
            }
            if (!has_any) continue;
            int base = (int)yytable.size();
            yypact[s] = base;
            for (int t = 0; t < nT; t++) {
                int a = l_.action(s, t);
                int enc;
                int ck;
                if (a == 0) {
                    enc = NINF; ck = -1; // hole -> goto default
                } else if (a == LALR::ACCEPT) {
                    enc = 0; ck = t;     // not normally hit; yystate==YYFINAL handles accept
                } else if (a == LALR::ERR_MARK) {
                    enc = NINF; ck = t;  // explicit error (from %nonassoc) -> yytable_value_is_error
                } else if (a > 0) {
                    enc = a - 1; ck = t; // shift dst state
                } else {
                    enc = a; ck = t;     // reduce: -rule
                }
                yytable.push_back(enc);
                yycheck.push_back(ck);
            }
        }
        // Goto table: per-nonterminal row of nS entries.
        for (int nt = 0; nt < nN; nt++) {
            bool has_any = false;
            for (int s = 0; s < nS; s++) if (l_.goto_tab(s, nt) != 0) { has_any = true; break; }
            if (!has_any) { yypgoto[nt] = NINF; yydefgoto[nt] = 0; continue; }
            int base = (int)yytable.size();
            yypgoto[nt] = base;
            // Pick a default goto: most frequent non-zero target (bison style).
            std::map<int, int> freq;
            for (int s = 0; s < nS; s++) {
                int g = l_.goto_tab(s, nt);
                if (g != 0) freq[g]++;
            }
            int default_goto = 0, best = 0;
            for (auto& [g, n] : freq) if (n > best) { best = n; default_goto = g; }
            yydefgoto[nt] = (default_goto == 0) ? 0 : (default_goto - 1);
            for (int s = 0; s < nS; s++) {
                int g = l_.goto_tab(s, nt);
                if (g == 0 || g == default_goto) {
                    yytable.push_back(NINF); yycheck.push_back(-1);
                } else {
                    yytable.push_back(g - 1); yycheck.push_back(s);
                }
            }
        }
        out << "#define YYPACT_NINF " << NINF << "\n";
        out << "#define yypact_value_is_default(Yyn) ((Yyn) == YYPACT_NINF)\n";
        out << "#define YYTABLE_NINF " << NINF << "\n";
        out << "#define yytable_value_is_error(Yyn) ((Yyn) == YYTABLE_NINF)\n";
        out << "#define YYTABLE_SIZE (sizeof(yytable)/sizeof(yytable[0]))\n";
        emit_array(out, "short", "yypact", yypact);
        emit_array(out, "short", "yypgoto", yypgoto);
        emit_array(out, "short", "yydefgoto", yydefgoto);
        emit_array(out, "short", "yytable", yytable);
        emit_array(out, "short", "yycheck", yycheck);
    }

    void emit_misc_tables(Buf out) {
        emit_array(out, "short", "yydefact", l_.default_reductions());
        emit_array(out, "short", "yystos", l_.stos_internal());
        // yyr1: lhs internal per rule
        vector<int> r1;
        for (int p = 0; p < l_.n_rules(); p++) r1.push_back(l_.prod_lhs_internal(p));
        emit_array(out, "short", "yyr1", r1);
        emit_array(out, "short", "yyr2", l_.rule_lengths());

        out << "#if YYDEBUG\n";
        emit_array(out, "short", "yyrline", l_.rule_lines());
        out << "#endif\n";

        // yytoknum: external code per terminal internal
        vector<int> tn;
        for (int i = 0; i < l_.n_terminals(); i++) tn.push_back(l_.term_external_code(i));
        emit_array(out, "int", "yytoknum", tn);

        // yytname: symbol display names per internal index
        out << "static const char * const yytname[] = {\n";
        for (int i = 0; i < l_.n_total_syms(); i++) {
            int s = l_.internal_to_sym(i);
            string nm = (s == g_.eof_sym) ? "$end" : g_.syms[s].name;
            out << "  \"" << esc(nm) << "\",\n";
        }
        out << "  0\n};\n";
    }

    void emit_yyerror_default(Buf out) {
        out <<
            "#if !defined YYERROR_USER_PROVIDED\n"
            "#if defined(__GNUC__) || defined(__clang__)\n"
            "__attribute__((weak))\n"
            "#endif\n"
            "void yyerror(const char *msg) { (void)fprintf(stderr, \"%s\\n\", msg); }\n"
            "#endif\n";
    }

    void emit_driver(Buf out) {
        const bool L = g_.want_locations;
        out << "extern int yylex(void);\n";
        out << "#define YYTRANSLATE(c) ((0 <= (c) && (c) <= YYMAXUTOK) ? yytranslate[c] : 257)\n";
        if (L) {
            out << "#ifndef YYLLOC_DEFAULT\n";
            out << "# define YYLLOC_DEFAULT(Cur, Rhs, N)                              \\\n";
            out << "    do                                                            \\\n";
            out << "      if (N) {                                                    \\\n";
            out << "        (Cur).first_line   = YYRHSLOC(Rhs, 1).first_line;         \\\n";
            out << "        (Cur).first_column = YYRHSLOC(Rhs, 1).first_column;       \\\n";
            out << "        (Cur).last_line    = YYRHSLOC(Rhs, N).last_line;          \\\n";
            out << "        (Cur).last_column  = YYRHSLOC(Rhs, N).last_column;        \\\n";
            out << "      } else {                                                    \\\n";
            out << "        (Cur).first_line   = (Cur).last_line   =                  \\\n";
            out << "          YYRHSLOC(Rhs, 0).last_line;                             \\\n";
            out << "        (Cur).first_column = (Cur).last_column =                  \\\n";
            out << "          YYRHSLOC(Rhs, 0).last_column;                           \\\n";
            out << "      }                                                           \\\n";
            out << "    while (0)\n";
            out << "#endif\n";
            out << "#define YYRHSLOC(Rhs, K) ((Rhs)[K])\n";
        }
        out << "\n";
        out << "int yyparse(void) {\n";
        out << "    int yystate = 0;\n";
        out << "    int yyerrstatus = 0;\n";
        out << "    int yystacksize = YYINITDEPTH;\n";
        out << "    short *yyss = NULL;\n";
        out << "    YYSTYPE *yyvs = NULL;\n";
        if (L) out << "    YYLTYPE *yyls = NULL;\n";
        out << "    short yyssa[YYINITDEPTH];\n";
        out << "    YYSTYPE yyvsa[YYINITDEPTH];\n";
        if (L) out << "    YYLTYPE yylsa[YYINITDEPTH];\n";
        out << "    short *yyssp;\n";
        out << "    YYSTYPE *yyvsp;\n";
        if (L) out << "    YYLTYPE *yylsp;\n";
        out << "    int yyn;\n";
        out << "    int yyresult;\n";
        out << "    int yytoken = -2;\n";
        out << "    YYSTYPE yyval;\n";
        if (L) out << "    YYLTYPE yyloc;\n";
        out << "    int yylen = 0;\n";
        out << "    yyss = yyssa; yyvs = yyvsa;\n";
        if (L) out << "    yyls = yylsa;\n";
        out << "    yyssp = yyss; yyvsp = yyvs;\n";
        if (L) out << "    yylsp = yyls;\n";
        out << "    *yyssp = 0;\n";
        out << "    yychar = -2;\n";
        out << "    yynerrs = 0;\n";
        out << "    goto yysetstate;\n";
        out << "\n";
        out << "yynewstate:\n";
        out << "    yyssp++;\n";
        out << "yysetstate:\n";
        out << "    *yyssp = (short)yystate;\n";
        out << "    if (yyss + yystacksize - 1 <= yyssp) {\n";
        out << "        long yysize = yyssp - yyss + 1;\n";
        out << "        if (yystacksize >= YYMAXDEPTH) goto yyexhaustedlab;\n";
        out << "        yystacksize *= 2;\n";
        out << "        if (yystacksize > YYMAXDEPTH) yystacksize = YYMAXDEPTH;\n";
        out << "        short *new_ss = (short*)malloc((size_t)yystacksize * sizeof(short));\n";
        out << "        YYSTYPE *new_vs = (YYSTYPE*)malloc((size_t)yystacksize * sizeof(YYSTYPE));\n";
        if (L) out << "        YYLTYPE *new_ls = (YYLTYPE*)malloc((size_t)yystacksize * sizeof(YYLTYPE));\n";
        out << "        if (!new_ss || !new_vs" << (L ? " || !new_ls" : "")
            << ") { free(new_ss); free(new_vs);" << (L ? " free(new_ls);" : "") << " goto yyexhaustedlab; }\n";
        out << "        memcpy(new_ss, yyss, (size_t)yysize * sizeof(short));\n";
        out << "        memcpy(new_vs, yyvs, (size_t)yysize * sizeof(YYSTYPE));\n";
        if (L) out << "        memcpy(new_ls, yyls, (size_t)yysize * sizeof(YYLTYPE));\n";
        out << "        if (yyss != yyssa) { free(yyss); free(yyvs);" << (L ? " free(yyls);" : "") << " }\n";
        out << "        yyss = new_ss; yyvs = new_vs;" << (L ? " yyls = new_ls;" : "") << "\n";
        out << "        yyssp = yyss + yysize - 1;\n";
        out << "        yyvsp = yyvs + yysize - 1;\n";
        if (L) out << "        yylsp = yyls + yysize - 1;\n";
        out << "    }\n";
        out << "    if (yystate == YYFINAL) goto yyacceptlab;\n";
        out << "\n";
        out << "yybackup:\n";
        out << "    yyn = yypact[yystate];\n";
        out << "    if (yypact_value_is_default(yyn)) goto yydefault;\n";
        out << "    if (yychar == -2) yychar = yylex();\n";
        out << "    if (yychar <= 0) { yychar = 0; yytoken = 0; }\n";
        out << "    else if (yychar == 256) { yychar = 257; yytoken = YYTRANSLATE(257); goto yyerrlab1; }\n";
        out << "    else yytoken = YYTRANSLATE(yychar);\n";
        out << "    yyn += yytoken;\n";
        out << "    if (yyn < 0 || yyn >= (int)YYTABLE_SIZE || yycheck[yyn] != yytoken) goto yydefault;\n";
        out << "    yyn = yytable[yyn];\n";
        out << "    if (yyn <= 0) {\n";
        out << "        if (yytable_value_is_error(yyn)) goto yyerrlab;\n";
        out << "        if (yyn == 0) goto yyacceptlab;\n";
        out << "        yyn = -yyn;\n";
        out << "        goto yyreduce;\n";
        out << "    }\n";
        out << "    if (yyerrstatus) yyerrstatus--;\n";
        out << "    yystate = yyn;\n";
        out << "    *++yyvsp = yylval;\n";
        if (L) out << "    *++yylsp = yylloc;\n";
        out << "    yychar = -2;\n";
        out << "    goto yynewstate;\n";
        out << "\n";
        out << "yydefault:\n";
        out << "    yyn = yydefact[yystate];\n";
        out << "    if (yyn == 0) goto yyerrlab;\n";
        out << "    goto yyreduce;\n";
        out << "\n";
        out << "yyreduce:\n";
        out << "    yylen = yyr2[yyn];\n";
        out << "    if (yylen) yyval = yyvsp[1 - yylen];\n";
        out << "    else memset(&yyval, 0, sizeof(yyval));\n";
        if (L) out << "    YYLLOC_DEFAULT(yyloc, (yylsp - yylen), yylen);\n";
        out << "    switch (yyn) {\n";
    }

    void emit_action_switch(Buf out) {
        for (int i = 1; i < l_.n_rules(); i++) {
            const Production& p = l_.prod(i);
            if (p.action.empty()) continue;
            out << "    case " << i << ":\n";
            if (!opts_.no_lines && p.action_line > 0)
                out << "#line " << p.action_line << " \"" << g_.source_file << "\"\n";
            out << "      { " << translate_action(p) << " }\n";
            if (!opts_.no_lines)
                out << "#line " << "0" << " \"" << /*synthetic*/ "yacc-output" << "\"\n";
            out << "      break;\n";
        }
    }

    // Local-only "is identifier continuation": for $name / @name we want
    // alnum + underscore *but not '.'* (so "@t.first_line" parses @t then .first_line).
    static bool ref_idcont(unsigned char c) {
        return ch_isalnum(c) || c == '_';
    }

    string translate_action(const Production& p) {
        string out;
        const string& s = p.action;
        // For mid-rule synthetic productions: $k refers to the k-th symbol of
        // the *parent* rule, found at yyvsp[k - midrule_offset].
        // Use rhs_size = midrule_offset (for offset calc) when applicable.
        const int eff_rhs_size = (p.midrule_offset >= 0)
                                    ? p.midrule_offset
                                    : (int)p.rhs.size();
        size_t i = 0;
        while (i < s.size()) {
            char c = s[i];
            // Skip string and char literals verbatim — '$' and '@' inside them
            // are user data, not parser-generator references.
            if (c == '"' || c == '\'') {
                char quote = c;
                out += c; i++;
                while (i < s.size()) {
                    if (s[i] == '\\' && i + 1 < s.size()) {
                        out += s[i]; i++;
                        out += s[i]; i++;
                        continue;
                    }
                    out += s[i];
                    if (s[i] == quote) { i++; break; }
                    i++;
                }
                continue;
            }
            // Skip block and line comments verbatim.
            if (c == '/' && i + 1 < s.size() && s[i+1] == '/') {
                while (i < s.size() && s[i] != '\n') { out += s[i]; i++; }
                continue;
            }
            if (c == '/' && i + 1 < s.size() && s[i+1] == '*') {
                out += s[i++]; out += s[i++];
                while (i + 1 < s.size() && !(s[i] == '*' && s[i+1] == '/')) { out += s[i++]; }
                if (i + 1 < s.size()) { out += s[i++]; out += s[i++]; }
                continue;
            }
            // @-references: only expanded when locations are enabled.
            if (c == '@' && g_.want_locations && i + 1 < s.size()) {
                size_t j = i + 1;
                if (s[j] == '$') {
                    out += "(yyloc)";
                    i = j + 1; continue;
                }
                if (ch_isdigit((unsigned char)s[j]) || s[j] == '-') {
                    size_t k = j;
                    if (s[k] == '-') k++;
                    while (k < s.size() && ch_isdigit((unsigned char)s[k])) k++;
                    int n = std::atoi(s.substr(j, k - j).c_str());
                    int offset = n - eff_rhs_size;
                    out += "(yylsp[";
                    out += std::to_string(offset);
                    out += "])";
                    i = k; continue;
                }
                if (ch_isalpha((unsigned char)s[j]) || s[j] == '_') {
                    size_t k = j;
                    while (k < s.size() && ref_idcont((unsigned char)s[k])) k++;
                    string nm = s.substr(j, k - j);
                    if (!nm.empty() && nm == p.lhs_name) {
                        out += "(yyloc)";
                        i = k; continue;
                    }
                    int found = -1;
                    for (int idx = 0; idx < (int)p.rhs_names.size(); idx++)
                        if (p.rhs_names[idx] == nm && !nm.empty()) { found = idx; break; }
                    if (found >= 0) {
                        int offset = (found + 1) - (int)p.rhs.size();
                        out += "(yylsp[";
                        out += std::to_string(offset);
                        out += "])";
                        i = k; continue;
                    }
                }
                // Unknown @form: emit verbatim.
                out += c; i++; continue;
            }
            if (c != '$') { out += c; i++; continue; }
            string tag;
            size_t j = i + 1;
            if (j < s.size() && s[j] == '<') {
                size_t e = s.find('>', j);
                if (e != string::npos) { tag = s.substr(j + 1, e - j - 1); j = e + 1; }
            }
            if (j < s.size() && s[j] == '$') {
                out += "(yyval";
                if (!tag.empty()) out += "." + tag;
                else if (g_.has_union) {
                    if (!p.lhs_tag.empty()) out += "." + p.lhs_tag;
                }
                out += ")";
                i = j + 1; continue;
            }
            if (j < s.size() && (ch_isdigit((unsigned char)s[j]) || s[j] == '-')) {
                size_t k = j;
                if (s[k] == '-') k++;
                while (k < s.size() && ch_isdigit((unsigned char)s[k])) k++;
                int n = std::atoi(s.substr(j, k - j).c_str());
                int offset = n - eff_rhs_size;
                out += "(yyvsp[";
                out += std::to_string(offset);
                out += "]";
                if (!tag.empty()) out += "." + tag;
                else if (g_.has_union && n >= 1 && n <= (int)p.rhs_tags.size()) {
                    const string& t = p.rhs_tags[n - 1];
                    if (!t.empty()) out += "." + t;
                }
                out += ")";
                i = k; continue;
            }
            if (j < s.size() && (ch_isalpha((unsigned char)s[j]) || s[j] == '_')) {
                size_t k = j;
                while (k < s.size() && ref_idcont((unsigned char)s[k])) k++;
                string nm = s.substr(j, k - j);
                if (!nm.empty() && nm == p.lhs_name) {
                    out += "(yyval";
                    if (!tag.empty()) out += "." + tag;
                    else if (g_.has_union && !p.lhs_tag.empty()) out += "." + p.lhs_tag;
                    out += ")";
                    i = k; continue;
                }
                int found = -1;
                for (int idx = 0; idx < (int)p.rhs_names.size(); idx++)
                    if (p.rhs_names[idx] == nm && !nm.empty()) { found = idx; break; }
                if (found >= 0) {
                    int offset = (found + 1) - (int)p.rhs.size();
                    out += "(yyvsp[";
                    out += std::to_string(offset);
                    out += "]";
                    if (!tag.empty()) out += "." + tag;
                    else if (g_.has_union && !p.rhs_tags[found].empty())
                        out += "." + p.rhs_tags[found];
                    out += ")";
                    i = k; continue;
                }
            }
            out += c; i++;
        }
        return out;
    }

    void emit_driver_tail(Buf out) {
        const bool L = g_.want_locations;
        out << "    }\n";
        out << "    yyssp -= yylen;\n";
        out << "    yyvsp -= yylen;\n";
        if (L) out << "    yylsp -= yylen;\n";
        out << "    yylen = 0;\n";
        out << "    *++yyvsp = yyval;\n";
        if (L) out << "    *++yylsp = yyloc;\n";
        out << "    {\n";
        out << "        int yylhs_internal = yyr1[yyn];\n";
        out << "        int nt = yylhs_internal - YYNTOKENS;\n";
        out << "        int gpos = yypgoto[nt] + *yyssp;\n";
        out << "        if (gpos >= 0 && gpos < (int)YYTABLE_SIZE && yycheck[gpos] == *yyssp)\n";
        out << "            yystate = yytable[gpos];\n";
        out << "        else\n";
        out << "            yystate = yydefgoto[nt];\n";
        out << "    }\n";
        out << "    goto yynewstate;\n";
        out << "\n";
        out << "yyerrlab:\n";
        out << "    if (!yyerrstatus) { ++yynerrs; yyerror(\"syntax error\"); }\n";
        out << "yyerrorlab:\n";
        out << "    if (yyerrstatus == 3) {\n";
        out << "        if (yychar <= 0) goto yyabortlab;\n";
        out << "        yychar = -2;\n";
        out << "    }\n";
        out << "yyerrlab1:\n";
        out << "    yyerrstatus = 3;\n";
        out << "    for (;;) {\n";
        out << "        yyn = yypact[yystate];\n";
        out << "        if (!yypact_value_is_default(yyn)) {\n";
        out << "            int err_internal = " << l_.error_internal() << ";\n";
        out << "            int idx = yyn + err_internal;\n";
        out << "            if (idx >= 0 && idx < (int)YYTABLE_SIZE && yycheck[idx] == err_internal) {\n";
        out << "                yyn = yytable[idx];\n";
        out << "                if (yyn > 0) {\n";
        out << "                    yystate = yyn;\n";
        out << "                    *++yyvsp = yylval;\n";
        if (L) out << "                    *++yylsp = yylloc;\n";
        out << "                    goto yynewstate;\n";
        out << "                }\n";
        out << "            }\n";
        out << "        }\n";
        out << "        if (yyssp == yyss) goto yyabortlab;\n";
        out << "        yyvsp--;\n";
        if (L) out << "        yylsp--;\n";
        out << "        yystate = *--yyssp;\n";
        out << "    }\n";
        out << "\n";
        out << "yyacceptlab:\n";
        out << "    yyresult = 0; goto yyreturn;\n";
        out << "yyabortlab:\n";
        out << "    yyresult = 1; goto yyreturn;\n";
        out << "yyexhaustedlab:\n";
        out << "    yyerror(\"memory exhausted\");\n";
        out << "    yyresult = 2; goto yyreturn;\n";
        out << "yyreturn:\n";
        out << "    if (yyss != yyssa) { free(yyss); free(yyvs);" << (L ? " free(yyls);" : "") << " }\n";
        out << "    return yyresult;\n";
        out << "}\n";
    }
};

// ============================================================================
// CLI driver
// ============================================================================

static int run(int argc, char** argv) {
    Options opts;
    string input;
    string out_arg;
    string defines_path_arg;
    string file_prefix;

    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        auto starts = [&](const char* p){ return a.rfind(p, 0) == 0; };
        if (a == "-d") opts.want_header = true;
        else if (a == "--defines") opts.want_header = true;
        else if (starts("--defines=")) { opts.want_header = true; defines_path_arg = a.substr(10); }
        else if (a == "-H" || a == "--header") opts.want_header = true;
        else if (starts("--header=")) { opts.want_header = true; defines_path_arg = a.substr(9); }
        else if (a == "-o") { if (i + 1 < argc) out_arg = argv[++i]; }
        else if (starts("--output=")) out_arg = a.substr(9);
        else if (a == "-y" || a == "--yacc") opts.yacc_compat = true;
        else if (a == "-l" || a == "--no-lines") opts.no_lines = true;
        else if (a == "-k" || a == "--token-table") opts.token_table = true;
        else if (a == "-v" || a == "--verbose") opts.verbose = true;
        else if (a == "-t" || a == "--debug") opts.debug = true;
        else if (a == "-V" || a == "--version") {
            write_stdout("yacc.cpp 0.1.0 (bison-compatible parser generator)\n");
            return 0;
        }
        else if (a == "-h" || a == "--help") {
            write_stdout("Usage: yacc [OPTION]... FILE\n");
            return 0;
        }
        else if (starts("-W")) {}
        else if (starts("--color")) {}
        else if (starts("-D") || starts("--define")) {}
        else if (starts("-F") || starts("--force-define")) {}
        else if (starts("-L") || starts("--language")) {}
        else if (starts("-S") || starts("--skeleton")) {}
        else if (starts("-b") || starts("--file-prefix")) {
            if (a == "-b" || a == "--file-prefix") { if (i + 1 < argc) file_prefix = argv[++i]; }
            else if (starts("-b")) file_prefix = a.substr(2);
            else file_prefix = a.substr(a.find('=') + 1);
        }
        else if (a == "-p") { if (i + 1 < argc) opts.name_prefix = argv[++i]; }
        else if (starts("-p")) opts.name_prefix = a.substr(2);
        else if (starts("-")) { /* ignore unknown */ }
        else input = a;
    }
    if (input.empty()) fatal("no input file specified");

    string src = load_input_file(input);
    Grammar g;
    GrammarParser gp(std::move(src), input, g, opts);
    if (!defines_path_arg.empty()) opts.defines_path = defines_path_arg;
    if (!file_prefix.empty()) opts.file_prefix = file_prefix;
    gp.parse();
    LALR la(g);
    la.build();

    string outpath, headerpath;
    if (out_arg.empty()) {
        if (opts.yacc_compat) { outpath = "y.tab.c"; headerpath = "y.tab.h"; }
        else if (!opts.file_prefix.empty()) {
            outpath = opts.file_prefix + ".tab.c"; headerpath = opts.file_prefix + ".tab.h";
        } else {
            string base = input;
            size_t slash = base.find_last_of("/\\");
            string dir = (slash == string::npos) ? "" : base.substr(0, slash + 1);
            string fname = (slash == string::npos) ? base : base.substr(slash + 1);
            size_t dot = fname.find_last_of('.');
            string stem = (dot == string::npos) ? fname : fname.substr(0, dot);
            outpath = dir + stem + ".tab.c";
            headerpath = dir + stem + ".tab.h";
        }
    } else {
        outpath = out_arg;
        size_t dot = outpath.find_last_of('.');
        headerpath = (dot == string::npos) ? outpath + ".h" : outpath.substr(0, dot) + ".h";
    }
    if (!opts.defines_path.empty()) headerpath = opts.defines_path;

    std::string out_s, hdr_s;
    Buf out_buf{out_s};
    Buf hdr_buf{hdr_s};
    Emitter em(g, la, opts);
    em.emit(out_buf, opts.want_header ? &hdr_buf : nullptr);

    if (!write_file(outpath, out_s))
        fatalf("cannot write output '{}'", outpath);
    if (opts.want_header && !write_file(headerpath, hdr_s))
        fatalf("cannot write header '{}'", headerpath);

    if (la.sr_conflicts() > 0)
        write_stderr(std::format("yacc: {} shift/reduce conflict(s)\n", la.sr_conflicts()));
    if (la.rr_conflicts() > 0)
        write_stderr(std::format("yacc: {} reduce/reduce conflict(s)\n", la.rr_conflicts()));
    return 0;
}

// Fuzzer entry point: parse + LALR build + emit, on a UTF-8 byte buffer.
// Returns 0 on success, nonzero on error. Discards output.
int fuzz_run_buffer(const char* data, size_t len) {
    try {
        Options opts;
        Grammar g;
        std::string s(data, len);
        GrammarParser gp(std::move(s), "<fuzz>", g, opts);
        gp.parse();
        LALR la(g);
        la.build();
        std::string out_s;
        Buf out_buf{out_s};
        Emitter em(g, la, opts);
        em.emit(out_buf, nullptr);
        return 0;
    } catch (const YaccError&) { return 1; }
    catch (const std::exception&) { return 2; }
    catch (...) { return 3; }
}

int yacc_main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const YaccError& e) {
        write_stderr(std::string("yacc: error: ") + e.what() + "\n");
        return 1;
    }
}

} // namespace yacc
