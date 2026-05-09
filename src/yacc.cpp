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
#include <unordered_set>
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

    // GLR-only:
    int dprec = 0;          // %dprec N: dynamic precedence at merge points.
    string merge_fn;        // %merge <fn>: user function combining two values.
};

struct Grammar {
    vector<Symbol> syms;
    vector<Production> prods;
    int start_sym = -1;
    int eof_sym = -1;
    int error_sym = -1;
    int undef_sym = -1;
    int accept_sym = -1;

    string prologue;       // %{ ... %} blocks BEFORE the first typed
                           // declaration; emitted in source BEFORE YYSTYPE.
    string prologue_late;  // %{ ... %} blocks AFTER the first typed
                           // declaration; emitted in source AFTER YYSTYPE
                           // (so they may reference YYSTYPE freely).
    string prologue_requires;
    string prologue_provides;
    string prologue_top;
    string epilogue;
    string union_body;
    bool has_union = false;
    string api_value_type;
    string api_value_union_name;
    // %define api.location.type {custom_type} replaces the default 4-int
    // YYLTYPE struct with `typedef custom_type YYLTYPE;`.
    string api_location_type;
    bool want_locations = false;
    // %define parse.error: simple (default) | verbose | detailed | custom
    string parse_error_mode = "simple";
    // %define api.pure / %pure-parser: false (default) | true | full
    string api_pure = "false";
    // %define api.token.raw: when true, token enum values equal internal
    // indices and YYTRANSLATE is the identity, removing the need for the
    // external-to-internal yytranslate[] lookup.
    bool api_token_raw = false;
    // %define lr.type: lalr (default) | ielr | canonical-lr
    // Selects the parser-table construction algorithm.  canonical-lr
    // produces strictly more states than LALR (full LR(1) item sets);
    // LALR merges states with identical cores but different lookaheads.
    string lr_type = "lalr";
    // %glr-parser: emit a GLR runtime instead of the deterministic LALR
    // driver.  Conflicts are kept as multiple parse-paths run lock-step,
    // resolved at merge points by %dprec (higher wins) or %merge function.
    bool is_glr = false;
    // %define parse.lac: none (default) | full
    // When full, the verbose error helper does an exploratory parse so the
    // expected-token list excludes tokens that would default-reduce and
    // then fail in a later state.  Only affects diagnostics.
    string parse_lac = "none";
    // %define api.push-pull: pull (default) | push | both
    // - pull: only yyparse() is generated (calls yylex internally).
    // - push: only yypstate_new/yypush_parse/yypstate_delete are generated.
    // - both: both APIs are generated (yyparse() runs an internal pull loop).
    string api_push_pull = "pull";
    // %parse-param {type name} ... — appended to yyparse signature
    // %lex-param   {type name} ... — passed to yylex calls (pure parsers)
    // Stored as raw "type name" strings, one per brace block.
    vector<string> parse_params;
    vector<string> lex_params;

    // %destructor / %printer: per-symbol code body.  Defaults indexed by tag,
    // with "*" for "all typed symbols" and "" for "all untyped symbols".
    std::map<int, string> destructor_by_sym;
    std::map<int, string> printer_by_sym;
    std::map<string, string> destructor_default;  // tag -> body; tag is "<>", "<*>", or a real tag
    std::map<string, string> printer_default;

    // %initial-action — body to emit at the top of yyparse().
    string initial_action;

    // %define parse.trace = true (or -t flag).  Enables YYDEBUG=1 by default
    // and emits the YYDPRINTF / YY_SYMBOL_PRINT / YY_REDUCE_PRINT / YY_STACK_PRINT
    // helpers and calls.
    bool parse_trace = false;
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
    bool want_graph = false;     // -g
    bool want_xml = false;       // -x
    bool want_counterexamples = false; // -Wcounterexamples
    string graph_path;           // optional path passed to -g
    string xml_path;             // optional path passed to -x
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
            {"pure_parser", Tok::PercentPureParser},  /* old underscore variant */
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

        while (peek_.kind != Tok::EndOfFile && peek_.kind != Tok::DoublePercent) {
            // Bison's own parse-gram.y interleaves %type/%printer/%destructor
            // declarations with rules.  When we don't see an identifier-colon,
            // try the declaration parser.
            if (peek_.kind == Tok::Identifier_Colon)
                parse_rule();
            else
                parse_declaration();
        }

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

    // Bison's parse-gram.y uses _("string") for translatable token aliases.
    // We treat _("...") as equivalent to a bare "..." in alias positions.
    bool at_alias_string() const {
        return at(Tok::StrLit)
            || (at(Tok::Identifier) && peek_.text == "_");
    }
    string consume_alias_string() {
        if (at(Tok::StrLit)) { string s = peek_.text; advance(); return s; }
        // _("string")
        advance();
        if (!(at(Tok::Punct) && peek_.text == "("))
            fatalf("expected '(' after '_' alias wrapper at line {}", peek_.line);
        advance();
        if (!at(Tok::StrLit))
            fatalf("expected string literal in _(...) at line {}", peek_.line);
        string s = peek_.text;
        advance();
        if (!(at(Tok::Punct) && peek_.text == ")"))
            fatalf("expected ')' to close _(...) at line {}", peek_.line);
        advance();
        return s;
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
        // Stray ';' between declarations is allowed (Bison treats it as
        // an empty declaration -- their parse-gram.y emits "%code requires
        // {...};" with a trailing semicolon).
        if (t.kind == Tok::Semi) { advance(); return; }
        switch (t.kind) {
            case Tok::PercentBraces:
                if (seen_typed_decl_) g_.prologue_late += t.text;
                else                   g_.prologue      += t.text;
                advance();
                return;
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
                // Old-style PostgreSQL form: %name-prefix="base_yy"
                if (at(Tok::Punct) && peek_.text == "=") advance();
                if (at(Tok::StrLit)) { g_.api_prefix = peek_.text; advance(); }
                return;
            case Tok::PercentRequire:
                advance();
                if (at(Tok::StrLit)) {
                    // Compare requested "X.Y[.Z]" against our reported
                    // version.  Fail if the user requires a strictly higher
                    // version.  Bison accepts lower versions silently.
                    // We report a high version (3.8.0) so existing grammars
                    // that say %require "3.0" or similar continue to work.
                    static const int our_major = 3, our_minor = 8, our_patch = 0;
                    int rmaj = 0, rmin = 0, rpat = 0;
                    const string& v = peek_.text;
                    size_t p = 0;
                    auto parse_int = [&]() {
                        int n = 0;
                        while (p < v.size() && ch_isdigit((unsigned char)v[p])) {
                            n = n * 10 + (v[p] - '0'); p++;
                        }
                        return n;
                    };
                    rmaj = parse_int();
                    if (p < v.size() && v[p] == '.') { p++; rmin = parse_int(); }
                    if (p < v.size() && v[p] == '.') { p++; rpat = parse_int(); }
                    bool too_high =
                        (rmaj > our_major) ||
                        (rmaj == our_major && rmin > our_minor) ||
                        (rmaj == our_major && rmin == our_minor && rpat > our_patch);
                    if (too_high)
                        fatalf("require {} but yacc.cpp is 3.8.0", v);
                    advance();
                }
                return;
            case Tok::PercentLanguage:
            case Tok::PercentSkeleton:
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
                advance();
                g_.api_pure = "full";
                return;
            case Tok::PercentGlrParser:
                advance();
                g_.is_glr = true;
                return;
            case Tok::PercentToken_Table: advance(); opts_.token_table = true; return;
            case Tok::PercentNoLines: advance(); opts_.no_lines = true; return;
            case Tok::PercentParseParam:
            case Tok::PercentLexParam:
            case Tok::PercentParam: {
                Tok which = t.kind;
                advance();
                // Bison accepts multiple {TYPE NAME} blocks after one directive.
                while (at(Tok::BraceBlock)) {
                    string body = peek_.text;
                    // Strip leading/trailing whitespace.
                    size_t a = body.find_first_not_of(" \t\r\n");
                    size_t b = body.find_last_not_of(" \t\r\n");
                    body = (a == string::npos) ? string() : body.substr(a, b - a + 1);
                    if (which != Tok::PercentLexParam)
                        g_.parse_params.push_back(body);
                    if (which != Tok::PercentParseParam)
                        g_.lex_params.push_back(body);
                    advance();
                }
                return;
            }
            case Tok::PercentDestructor:
            case Tok::PercentPrinter: {
                bool is_destructor = (t.kind == Tok::PercentDestructor);
                advance();
                // Optional <tag> applies the body to one specific tag.
                string explicit_tag;
                bool has_explicit_tag = false;
                if (at(Tok::Tag)) { explicit_tag = peek_.text; has_explicit_tag = true; advance(); }
                if (!at(Tok::BraceBlock))
                    fatalf("%{} requires a {{ body }} at line {}",
                           (is_destructor ? "destructor" : "printer"), peek_.line);
                string body = peek_.text;
                advance();
                // Followed by either symbol identifiers/literals or <tag> selectors
                // (<*> for "all typed", <> for "all untyped", <name> for one tag).
                bool got_target = false;
                while (true) {
                    if (at(Tok::Identifier)) {
                        int idx = g_.find(peek_.text);
                        if (idx < 0) idx = g_.intern(peek_.text, false);
                        if (is_destructor) g_.destructor_by_sym[idx] = body;
                        else g_.printer_by_sym[idx] = body;
                        advance(); got_target = true;
                    } else if (at(Tok::CharLit)) {
                        int idx = sym_of_char(peek_);
                        if (is_destructor) g_.destructor_by_sym[idx] = body;
                        else g_.printer_by_sym[idx] = body;
                        advance(); got_target = true;
                    } else if (at(Tok::StrLit)) {
                        int idx = sym_of_strlit(peek_.text);
                        if (is_destructor) g_.destructor_by_sym[idx] = body;
                        else g_.printer_by_sym[idx] = body;
                        advance(); got_target = true;
                    } else if (at(Tok::Tag)) {
                        // <tag>, <*>, <>
                        string tag = peek_.text;
                        if (is_destructor) g_.destructor_default[tag] = body;
                        else g_.printer_default[tag] = body;
                        advance(); got_target = true;
                    } else break;
                }
                if (!got_target && has_explicit_tag) {
                    // %destructor <tag> { body }   (no symbol list)
                    if (is_destructor) g_.destructor_default[explicit_tag] = body;
                    else g_.printer_default[explicit_tag] = body;
                }
                (void)explicit_tag;
                return;
            }
            case Tok::PercentInitialAction:
                advance();
                if (at(Tok::BraceBlock)) {
                    g_.initial_action = peek_.text;
                    advance();
                }
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
            advance();
            // Stitch hyphenated values like "canonical-lr" or "push-pull"
            // (the lexer split on the dash) back together.
            while (at(Tok::Punct) && peek_.text == "-") {
                v += "-";
                advance();
                if (at(Tok::Identifier)) { v += peek_.text; advance(); }
                else break;
            }
            if (name == "api.value.type") g_.api_value_type = v;
            else if (name == "api.prefix") g_.api_prefix = v;
            else if (name == "api.token.prefix") g_.token_prefix = v;
            else if (name == "parse.error") g_.parse_error_mode = v;
            else if (name == "api.pure") g_.api_pure = v;
            else if (name == "parse.trace") {
                g_.parse_trace = (v == "true" || v == "1" || v == "on");
            }
            else if (name == "api.token.raw") {
                g_.api_token_raw = (v == "true" || v == "1");
            }
            else if (name == "api.push-pull") g_.api_push_pull = v;
            else if (name == "parse.lac") g_.parse_lac = v;
            else if (name == "lr.type") g_.lr_type = v;
            else if (name == "api.location.type") g_.api_location_type = v;
        } else if (at(Tok::BraceBlock)) {
            // Braced value: keep raw braces for api.value.type but strip
            // them for prefix-like settings whose value is the body.
            string raw = peek_.text;
            string trimmed;
            for (char c : raw) if (c != ' ' && c != '\t' && c != '\r' && c != '\n') trimmed += c;
            (void)trimmed;
            string body = raw;
            // Trim leading/trailing whitespace from body.
            size_t a = body.find_first_not_of(" \t\r\n");
            size_t b = body.find_last_not_of(" \t\r\n");
            body = (a == string::npos) ? string() : body.substr(a, b - a + 1);
            if (name == "api.value.type") g_.api_value_type = "{" + raw + "}";
            else if (name == "api.prefix") g_.api_prefix = body;
            else if (name == "api.token.prefix") g_.token_prefix = body;
            else if (name == "api.location.type") g_.api_location_type = body;
            else if (name == "api.push-pull") g_.api_push_pull = body;
            advance();
        } else {
            // %define NAME (no value) — treat as "true".
            if (name == "api.pure") g_.api_pure = "true";
            else if (name == "parse.trace") g_.parse_trace = true;
            else if (name == "api.token.raw") g_.api_token_raw = true;
        }
    }

    void parse_code() {
        string qual;
        if (at(Tok::Identifier)) {
            qual = peek_.text;
            advance();
            // Stitch hyphenated qualifiers like "pre-printer" / "post-printer"
            // (used in Bison's own parse-gram.y).
            while (at(Tok::Punct) && peek_.text == "-") {
                qual += "-";
                advance();
                if (at(Tok::Identifier)) { qual += peek_.text; advance(); }
                else break;
            }
        }
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
        seen_typed_decl_ = true;
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
        if (at(Tok::Tag)) { tag = peek_.text; advance(); seen_typed_decl_ = true; }
        while (true) {
            // Allow tag re-binding mid-list (matches Perl-style usage).
            if (at(Tok::Tag)) { tag = peek_.text; advance(); seen_typed_decl_ = true; continue; }
            int idx = -1;
            if (at(Tok::Identifier)) {
                idx = g_.find(peek_.text);
                if (idx < 0) idx = g_.intern(peek_.text, false);
                advance();
            } else if (at(Tok::CharLit)) {
                idx = sym_of_char(peek_); advance();
            } else if (at_alias_string()) {
                // Bison allows "alias" / _("alias") here to refer to a token
                // by its alias.  Resolve to the underlying token.
                idx = sym_of_strlit(consume_alias_string());
            } else break;
            // Follow alias_of so the type-tag lands on the real token, not
            // its alias entry.
            if (idx >= 0 && g_.syms[idx].alias_of >= 0)
                idx = g_.syms[idx].alias_of;
            if (!tag.empty()) g_.syms[idx].type_tag = tag;
        }
    }

    int next_prec_ = 0;
    void parse_token_decl(Assoc assoc) {
        string tag;
        if (at(Tok::Tag)) { tag = peek_.text; advance(); seen_typed_decl_ = true; }
        int prec_level = 0;
        if (assoc != Assoc::None) prec_level = ++next_prec_;
        bool any = false;
        while (true) {
            // Perl's perly.y switches tag mid-declaration:
            //   %left <ival> OROP <pval> PLUGIN_LOGICAL_OR_LOW_OP
            // Each <tag> applies to symbols that follow until the next.
            if (at(Tok::Tag)) { tag = peek_.text; advance(); seen_typed_decl_ = true; continue; }
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
                if (at_alias_string()) alias_str = consume_alias_string();
            } else if (at(Tok::CharLit)) {
                idx = sym_of_char(peek_); advance();
                if (at_alias_string()) consume_alias_string();
            } else if (at_alias_string()) {
                idx = sym_of_strlit(consume_alias_string());
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
                if (at(Tok::Int)) {
                    p.dprec = (int)peek_.ival;
                    advance();
                }
            } else if (at(Tok::PercentMerge)) {
                advance();
                if (at(Tok::Tag)) {
                    p.merge_fn = peek_.text;
                    advance();
                }
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
                    // Optional [name] after a mid-rule action binds the
                    // mid-rule's $$ to a name (Perl's perly.y uses this).
                    string mr_namedref;
                    if (at(Tok::BracketName)) {
                        mr_namedref = peek_.text;
                        advance();
                    }
                    p.rhs_names.push_back(mr_namedref);
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
    // True once we've seen %union or any typed %token/%type, after which
    // %{ ... %} blocks go into prologue_late (emitted after YYSTYPE).
    bool seen_typed_decl_ = false;
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
        // ielr's recognition power equals canonical-LR's (Denny/Malloy 2010);
        // they only differ in the size of the resulting tables.  Until we
        // implement the IELR state-splitting algorithm proper, route ielr
        // through the canonical builder so the parser is correct.
        if (g_.lr_type == "canonical-lr" || g_.lr_type == "ielr") {
            build_canonical_lr1();
        } else {
            build_lr0();
            compute_lookaheads();
        }
        build_action_goto();
    }

    int n_terminals()    const { return (int)term_internal_.size(); }
    int n_nonterminals() const { return (int)nonterm_internal_.size(); }
    int n_total_syms()   const { return n_terminals() + n_nonterminals(); }
    int n_states()       const { return (int)states_.size(); }
    int n_rules()        const { return (int)g_.prods.size(); }

    int sym_to_internal(int s) const { return sym_to_internal_[s]; }
    int internal_to_sym(int i) const { return internal_to_sym_[i]; }

    const State& state(int s) const { return states_[s]; }
    const vector<int>& term_internals()    const { return term_internal_; }
    const vector<int>& nonterm_internals() const { return nonterm_internal_; }
    const Grammar& grammar() const { return g_; }
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
    // Canonical LR(1) only: per-state expanded item lookaheads parallel
    // to State::items.  Empty for LALR builds (which use compute_lookaheads
    // and then expand via compute_ext_items at build_action_goto time).
    vector<vector<std::set<int>>> items_la_;
public:
    // Per-conflict info for counter-example reporting.  Each entry is
    // (state, terminal-internal-index, kind) where kind=1 is S/R, kind=2 is R/R.
    struct Conflict { int state; int term_internal; int kind; };
    vector<Conflict> conflicts;

    // GLR-only: alternate actions kept alongside the resolved primary
    // action.  Each entry: (state, term_internal, action) where the
    // action follows the same encoding as action_[] (positive = shift
    // dst+1, negative = reduce by -rule).  The GLR runtime queries
    // both action_[] and this side list to spawn parallel parse paths.
    struct GlrAction { int state; int term_internal; int action; };
    vector<GlrAction> glr_extra_actions;
private:

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

    // Canonical LR(1): build the full LR(1) automaton.  States are
    // identified by their LR(1) kernel (LR(0) items + per-item lookahead
    // sets); states with the same LR(0) core but different lookaheads stay
    // separate, unlike LALR which merges them.  Tables can be much larger.
    void build_canonical_lr1() {
        index_by_lhs();
        // Each state's kernel is parallel (kernel[i], la[i]).
        State s0;
        s0.kernel = { Item{0u, 0u} };
        s0.la = { std::set<int>{0} };  // {$end internal}
        // Compute LR(1) closure: items[] holds expanded set, items_la[]
        // holds parallel lookahead sets.  We reuse State::items but track
        // per-item lookaheads via a local vector.
        vector<std::set<int>> items_la_initial;
        s0.items = lr1_closure(s0.kernel, s0.la, items_la_initial);
        states_.push_back(std::move(s0));
        state_access_.push_back(-1);
        items_la_.push_back(items_la_initial);

        // Identity key: sorted list of (item, lookahead-set) pairs.
        auto make_key = [](const vector<Item>& k, const vector<std::set<int>>& la) {
            vector<std::pair<Item, std::set<int>>> v;
            v.reserve(k.size());
            for (size_t i = 0; i < k.size(); i++) v.push_back({k[i], la[i]});
            std::sort(v.begin(), v.end(),
                [](const auto& a, const auto& b) {
                    if (a.first.prod != b.first.prod) return a.first.prod < b.first.prod;
                    if (a.first.dot  != b.first.dot)  return a.first.dot  < b.first.dot;
                    return a.second < b.second;
                });
            return v;
        };

        std::map<vector<std::pair<Item, std::set<int>>>, int> by_kernel;
        by_kernel[make_key(states_[0].kernel, states_[0].la)] = 0;

        for (int s = 0; s < (int)states_.size(); s++) {
            // For each transition symbol X, advance dots and merge lookaheads.
            std::map<int, std::map<Item, std::set<int>>> next;
            const auto& items = states_[s].items;
            const auto& its_la = items_la_[s];
            for (size_t k = 0; k < items.size(); k++) {
                Item it = items[k];
                const auto& rhs = g_.prods[it.prod].rhs;
                if (it.dot >= rhs.size()) continue;
                int X = rhs[it.dot];
                Item advanced{it.prod, it.dot + 1};
                auto& la_for = next[X][advanced];
                for (int t : its_la[k]) la_for.insert(t);
            }
            for (auto& [X, kernel_map] : next) {
                vector<Item> kernel;
                vector<std::set<int>> kla;
                for (auto& [it, la] : kernel_map) { kernel.push_back(it); kla.push_back(la); }
                auto key = make_key(kernel, kla);
                int target;
                auto it = by_kernel.find(key);
                if (it == by_kernel.end()) {
                    target = (int)states_.size();
                    State st;
                    st.kernel = kernel;
                    st.la = kla;
                    vector<std::set<int>> new_items_la;
                    st.items = lr1_closure(kernel, kla, new_items_la);
                    states_.push_back(std::move(st));
                    state_access_.push_back(X);
                    items_la_.push_back(std::move(new_items_la));
                    by_kernel[key] = target;
                } else target = it->second;
                states_[s].trans[X] = target;
            }
        }
        // Detect final state.
        size_t aug_size = g_.prods[0].rhs.size();
        for (int i = 0; i < (int)states_.size(); i++)
            for (auto& it : states_[i].kernel)
                if (it.prod == 0 && it.dot == aug_size) final_state_ = i;
    }

    // LR(1) closure helper used by canonical-lr.  Given a kernel (vector
    // of items with parallel lookahead sets), returns the full closure as
    // items[] with parallel lookaheads in `out_la[]`.
    vector<Item> lr1_closure(const vector<Item>& kernel,
                             const vector<std::set<int>>& kernel_la,
                             vector<std::set<int>>& out_la) {
        std::map<Item, std::set<int>> closure;
        for (size_t i = 0; i < kernel.size(); i++) {
            for (int t : kernel_la[i]) closure[kernel[i]].insert(t);
        }
        bool changed = true;
        while (changed) {
            changed = false;
            auto snapshot = closure;
            for (auto& [it, la_set] : snapshot) {
                const auto& rhs = g_.prods[it.prod].rhs;
                if (it.dot >= rhs.size()) continue;
                int X = rhs[it.dot];
                if (g_.syms[X].is_terminal) continue;
                // Compute FIRST(beta · a) for each lookahead a.
                for (int p : by_lhs_[X]) {
                    Item ni{(uint32_t)p, 0};
                    for (int a : la_set) {
                        std::set<int> firstset;
                        bool nullable_seq = true;
                        first_of_seq(rhs, it.dot + 1, firstset, nullable_seq);
                        if (nullable_seq) firstset.insert(a);
                        size_t before = closure[ni].size();
                        for (int t : firstset) closure[ni].insert(t);
                        if (closure[ni].size() != before) changed = true;
                    }
                }
            }
        }
        vector<Item> items;
        out_la.clear();
        items.reserve(closure.size());
        out_la.reserve(closure.size());
        for (auto& [it, la_set] : closure) {
            items.push_back(it);
            out_la.push_back(la_set);
        }
        return items;
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
        // Canonical LR(1) build already has the full per-item lookaheads
        // attached to State::items via items_la_.  Just emit those directly.
        if (g_.lr_type == "canonical-lr" && !items_la_.empty()) {
            vector<vector<ExtItem>> R(states_.size());
            for (int s = 0; s < (int)states_.size(); s++) {
                R[s].reserve(states_[s].items.size());
                for (size_t k = 0; k < states_[s].items.size(); k++) {
                    R[s].push_back({states_[s].items[k], items_la_[s][k]});
                }
            }
            return R;
        }
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
                            conflicts.push_back({s, la, 1});
                            // GLR keeps both actions; LALR drops the
                            // reduce in favor of the shift.
                            if (g_.is_glr)
                                glr_extra_actions.push_back({s, la, red});
                            // default: prefer shift
                        }
                    } else {
                        // reduce/reduce
                        int existing_rule = -cur;
                        rr_conflicts_++;
                        conflicts.push_back({s, la, 2});
                        if (g_.is_glr) {
                            // Keep BOTH reduces.  The current cell stays
                            // as 'cur' (the earlier rule); the new one
                            // goes to the extras list.
                            glr_extra_actions.push_back({s, la, red});
                        }
                        if ((int)ei.core.prod < existing_rule) {
                            // The newer rule wins by index; demote cur.
                            if (g_.is_glr)
                                glr_extra_actions.push_back({s, la, cur});
                            action_[idx] = red;
                        }
                    }
                }
            }
        }
        // Default reductions: pick the most common reduce action in the
        // state and use it as yydefact[s].  Prune row entries that equal
        // this default; the runtime falls through to yydefact when the
        // yytable lookup misses.  This works for explicit-error tokens
        // too -- they'd previously error immediately, now they default-
        // reduce first, then fail on the post-reduce state lookup -- a
        // standard table-compression trade-off (delayed error detection
        // for tighter tables).  Skip when parse.lac=full or parse.error
        // is verbose/detailed, since those modes need precise per-token
        // expected-set walks of yypact.
        const bool keep_explicit =
            (g_.parse_lac == "full") ||
            (g_.parse_error_mode == "verbose") ||
            (g_.parse_error_mode == "detailed");
        for (int s = 0; s < nS; s++) {
            std::unordered_map<int, int> reduce_counts;  // -rule -> count
            int best_rule = 0, best_count = 0;
            for (int t = 0; t < nT; t++) {
                int a = action_[s * nT + t];
                if (a < 0 && a != ERR_MARK) {
                    int n = ++reduce_counts[a];
                    if (n > best_count) { best_count = n; best_rule = a; }
                }
            }
            if (best_rule == 0) continue;
            // Conservative mode: only default when no shifts and exactly
            // one reduce-rule appears (matches the previous behaviour and
            // preserves precise error reporting).
            if (keep_explicit) {
                bool shift = false;
                for (int t = 0; t < nT; t++) {
                    int a = action_[s * nT + t];
                    if (a > 0 && a != ACCEPT) { shift = true; break; }
                }
                if (shift || reduce_counts.size() != 1) continue;
            }
            default_reduce_[s] = -best_rule;
            for (int t = 0; t < nT; t++)
                if (action_[s * nT + t] == best_rule) action_[s * nT + t] = 0;
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

    // Pure parsers move yylval/yychar/yynerrs/yylloc out of globals.
    bool pure() const {
        return g_.api_pure == "true" || g_.api_pure == "full";
    }

    // ", type1 name1, type2 name2" — for appending to function signatures.
    string params_decl(const vector<string>& v) const {
        string s;
        for (auto& p : v) { s += ", "; s += p; }
        return s;
    }

    // Signature for yyparse — "void" if no params, else comma-joined.
    string params_decl_signature() const {
        if (g_.parse_params.empty()) return "void";
        string s;
        for (size_t i = 0; i < g_.parse_params.size(); i++) {
            if (i) s += ", ";
            s += g_.parse_params[i];
        }
        return s;
    }

    // ", name1, name2" — for appending to call sites.  Extracts the last
    // identifier-like token of each "type name" declaration.
    string params_call(const vector<string>& v) const {
        string s;
        for (auto& p : v) {
            size_t end = p.find_last_not_of(" \t\r\n");
            if (end == string::npos) continue;
            size_t start = end;
            while (start > 0 && (ch_isalnum((unsigned char)p[start - 1]) || p[start - 1] == '_'))
                start--;
            s += ", ";
            s += p.substr(start, end - start + 1);
        }
        return s;
    }

    // Argument list for a yylex(...) call from inside yyparse.
    string yylex_call_args() const {
        string s;
        bool first = true;
        if (pure()) {
            s += "&yylval";
            first = false;
            if (g_.want_locations) { s += ", &yylloc"; }
        }
        for (auto& p : g_.lex_params) {
            size_t end = p.find_last_not_of(" \t\r\n");
            if (end == string::npos) continue;
            size_t start = end;
            while (start > 0 && (ch_isalnum((unsigned char)p[start - 1]) || p[start - 1] == '_'))
                start--;
            if (!first) s += ", ";
            s += p.substr(start, end - start + 1);
            first = false;
        }
        return s;
    }

    // Extra arguments passed from yyerrlab to yysyntax_error so it can
    // format the message and forward to user yyerror with the right ABI.
    string yysyntax_error_extra_args(bool push = false) const {
        string s;
        if (g_.parse_lac == "full") {
            if (push) s += ", yyps->yyss, yyps->yyssp";
            else      s += ", yyss, yyssp";
        }
        if (pure() && g_.want_locations) s += push ? ", &yyps->yylloc" : ", &yylloc";
        for (auto& p : g_.parse_params) {
            size_t end = p.find_last_not_of(" \t\r\n");
            if (end == string::npos) continue;
            size_t start = end;
            while (start > 0 && (ch_isalnum((unsigned char)p[start - 1]) || p[start - 1] == '_'))
                start--;
            s += ", ";
            s += p.substr(start, end - start + 1);
        }
        return s;
    }

    // "name1, name2" — like params_call but without leading ", ".
    string params_call_no_leading_comma(const vector<string>& v) const {
        string s = params_call(v);
        if (s.size() >= 2 && s[0] == ',' && s[1] == ' ') s.erase(0, 2);
        return s;
    }

    // Argument list for a yyerror(...) call.  Pure parsers prepend &yylloc
    // when locations are enabled, then any %parse-param identifiers, then
    // the message string passed verbatim.
    string yyerror_call_args(string_view msg_expr) const {
        string s;
        if (pure() && g_.want_locations) s += "&yylloc, ";
        s += params_call(g_.parse_params);
        // params_call leaves a leading ", "; strip if needed.
        if (!s.empty() && s.front() == ',') s.erase(0, 2);
        if (!s.empty()) s += ", ";
        s += msg_expr;
        return s;
    }

    void emit(Buf out, Buf* hdr) {
        emit_prefix(out);
        if (hdr) emit_prefix(*hdr);

        // %define api.prefix / %name-prefix: rename every yy* symbol via
        // #define so the rest of the emitter can keep using literal names.
        if (!g_.api_prefix.empty()) {
            emit_api_prefix(out);
            if (hdr) emit_api_prefix(*hdr);
        }

        // Tokens & YYSTYPE: emitted in both header (if any) and source.
        std::string tokens_s, vt_s;
        Buf tokens{tokens_s};
        Buf value_type{vt_s};
        emit_token_kinds(tokens);
        emit_value_type(value_type);

        if (hdr) {
            *hdr << "#ifndef YY_TAB_H_INCLUDED\n# define YY_TAB_H_INCLUDED\n";
            *hdr << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n";
            *hdr << "#ifndef YYDEBUG\n# define YYDEBUG " << (g_.parse_trace ? 1 : 0) << "\n#endif\n";
            *hdr << "#if YYDEBUG\nextern int yydebug;\n#endif\n";
            if (!g_.prologue_requires.empty())
                *hdr << "/* %code requires */\n" << g_.prologue_requires << "\n";
            *hdr << tokens_s;
            *hdr << vt_s;
            if (!pure()) {
                *hdr << "extern YYSTYPE yylval;\n";
                if (g_.want_locations) *hdr << "extern YYLTYPE yylloc;\n";
            }
            const bool h_push_only = (g_.api_push_pull == "push");
            const bool h_push_both = (g_.api_push_pull == "both");
            if (!h_push_only) {
                *hdr << "int yyparse(" << params_decl_signature() << ");\n";
            }
            if (h_push_only || h_push_both) {
                *hdr << "#ifndef YYPUSH_MORE\n# define YYPUSH_MORE 4\n#endif\n";
                *hdr << "typedef struct yypstate yypstate;\n";
                *hdr << "yypstate *yypstate_new(void);\n";
                *hdr << "void yypstate_delete(yypstate *);\n";
                *hdr << "int yypush_parse(yypstate *, int, YYSTYPE const *";
                if (g_.want_locations) *hdr << ", YYLTYPE const *";
                for (auto& p : g_.parse_params) *hdr << ", " << p;
                *hdr << ");\n";
            }
            if (g_.parse_error_mode == "custom") {
                // Forward-declare the public custom-parse-error API so
                // user code (driver.c) can implement yyreport_syntax_error.
                *hdr << "typedef int yysymbol_kind_t;\n";
                *hdr << "typedef struct yypcontext_s yypcontext_t;\n";
                *hdr << "yysymbol_kind_t yypcontext_token(const yypcontext_t *);\n";
                *hdr << "int yypcontext_expected_tokens(const yypcontext_t *, yysymbol_kind_t *, int);\n";
                if (g_.want_locations)
                    *hdr << "const YYLTYPE *yypcontext_location(const yypcontext_t *);\n";
                *hdr << "const char *yysymbol_name(yysymbol_kind_t);\n";
                // User must implement this:
                *hdr << "extern int yyreport_syntax_error(const yypcontext_t *";
                for (auto& p : g_.parse_params) *hdr << ", " << p;
                *hdr << ");\n";
            }
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
        // %{ %} blocks placed AFTER %union (or any typed decl) — bison's
        // "second part of user prologue".  These may freely reference
        // YYSTYPE because the typedef is already in scope.
        if (!g_.prologue_late.empty()) {
            if (!opts_.no_lines) out << "#line 1 \"" << g_.source_file << "\"\n";
            out << g_.prologue_late << "\n";
        }

        emit_constants(out);
        emit_translation_table(out);
        emit_compressed_tables(out);
        emit_misc_tables(out);
        emit_yyerror_default(out);
        emit_destructor(out);
        emit_printer(out);
        emit_trace_macros(out);
        // GLR replaces yyparse with a tree-of-stacks runtime; otherwise
        // push-only replaces it with the push API; otherwise the
        // standard pull driver, optionally augmented by the push API
        // (api.push-pull=both).
        const bool push_only = (g_.api_push_pull == "push");
        const bool push_both = (g_.api_push_pull == "both");
        if (g_.is_glr) {
            emit_glr_driver(out);
        } else {
            if (!push_only) {
                emit_driver(out);
                emit_action_switch(out);
                emit_driver_tail(out);
            }
            if (push_only || push_both) {
                emit_push_driver(out);
            }
        }

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

    // Emit `#define yy* <prefix>*` for each yy-prefixed symbol the parser
    // exposes.  Bison uses this same trick — the rest of the generated code
    // keeps writing literal `yyparse` / `yylval` and the preprocessor does
    // the renaming.  Internal labels (yynewstate etc.) are local to the
    // function body so the macro replacement is harmless there.
    void emit_api_prefix(Buf out) {
        // The user prefix REPLACES "yy".  So `%define api.prefix {foo_}`
        // makes yyparse → foo_parse, yylval → foo_lval, etc.
        const string& p = g_.api_prefix;
        static const char* names[] = {
            "parse", "lex", "error", "lval", "char", "nerrs", "debug",
            "lloc", "tname", "table", "check", "pact", "pgoto", "defgoto",
            "defact", "stos", "r1", "r2", "rline", "translate", "toknum",
            "destruct", "symbol_print", "syntax_error",
            nullptr
        };
        for (int i = 0; names[i]; i++) {
            out << "#define yy" << names[i] << " " << p << names[i] << "\n";
        }
        // Also rename the YY*-prefixed types so two parsers in one
        // translation unit don't collide on YYSTYPE / YYLTYPE / YYDEBUG.
        // Bison uses the uppercased user prefix here.  Numeric constants
        // like YYNTOKENS are file-local #defines that don't escape to the
        // linker, so we leave those alone (renaming them would re-define
        // the macros twice and collide with the constant emission below).
        string up = p;
        for (char& c : up) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        static const char* upper_names[] = {
            "STYPE", "LTYPE", "DEBUG", "TOKENTYPE", nullptr
        };
        for (int i = 0; upper_names[i]; i++) {
            out << "#define YY" << upper_names[i] << " " << up << upper_names[i] << "\n";
        }
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

    // Code reported in the token enum / #define for terminal s.
    int emit_code_for(int s) const {
        // With api.token.raw, the token "code" returned by the lexer is the
        // internal symbol index (matches yytable / yycheck columns directly).
        if (g_.api_token_raw) return l_.sym_to_internal(s);
        return g_.syms[s].code;
    }

    void emit_token_kinds(Buf out) {
        const string& tp = g_.token_prefix;
        out << "#ifndef YYTOKENTYPE\n# define YYTOKENTYPE\n";
        out << "  enum yytokentype {\n";
        out << "    YYEMPTY = -2,\n";
        out << "    YYEOF = " << (g_.api_token_raw ? l_.eof_internal() : 0) << ",\n";
        out << "    YYerror = " << (g_.api_token_raw ? l_.error_internal() : 256) << ",\n";
        out << "    YYUNDEF = " << (g_.api_token_raw ? l_.undef_internal() : 257);
        for (int s = 0; s < (int)g_.syms.size(); s++) {
            const Symbol& sm = g_.syms[s];
            if (!sm.is_terminal) continue;
            if (s == g_.eof_sym || s == g_.error_sym || s == g_.undef_sym) continue;
            if (sm.alias_of >= 0) continue;
            if (sm.name.empty()) continue;
            if (sm.name[0] == '\'' || sm.name[0] == '"' || sm.name[0] == '$') continue;
            out << ",\n    " << tp << sm.name << " = " << emit_code_for(s);
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
            out << "#ifndef " << tp << sm.name << "\n# define " << tp << sm.name
                << " " << emit_code_for(s) << "\n#endif\n";
        }
    }

    void emit_value_type(Buf out) {
        // Guard only on YYSTYPE_IS_DECLARED — checking !defined YYSTYPE
        // is broken when api.prefix is in effect, because the macro
        // `#define YYSTYPE <UP>STYPE` makes `defined YYSTYPE` true and
        // skips the typedef.  YYSTYPE_IS_DECLARED is unique per-include
        // and not subject to the rename.
        out << "#ifndef YYSTYPE_IS_DECLARED\n";
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
            out << "#ifndef YYLTYPE_IS_DECLARED\n";
            if (!g_.api_location_type.empty()) {
                // User-supplied type via %define api.location.type {T}.
                // Bison's convention: YYLTYPE becomes a typedef for T.
                // The user is responsible for declaring T in a %code requires
                // block (or earlier in the prologue) so it's visible here.
                out << "typedef " << g_.api_location_type << " YYLTYPE;\n";
            } else {
                out << "typedef struct YYLTYPE {\n";
                out << "  int first_line; int first_column;\n";
                out << "  int last_line;  int last_column;\n";
                out << "} YYLTYPE;\n";
                out << "# define YYLTYPE_IS_TRIVIAL 1\n";
            }
            out << "# define YYLTYPE_IS_DECLARED 1\n";
            out << "#endif\n";
        }
    }

    void emit_constants(Buf out) {
        if (!pure()) {
            out << "YYSTYPE yylval;\nint yychar;\nint yynerrs;\n";
            if (g_.want_locations)
                out << "YYLTYPE yylloc;\n";
        }
        out << "#ifndef YYDEBUG\n# define YYDEBUG " << (g_.parse_trace ? 1 : 0) << "\n#endif\n";
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
        // YYTRANSLATE — always emitted so push-only mode (which skips
        // emit_driver) still has it available.
        if (g_.api_token_raw) {
            out << "#define YYTRANSLATE(c) (c)\n";
        } else {
            out << "#define YYTRANSLATE(c) ((0 <= (c) && (c) <= YYMAXUTOK) ? yytranslate[c] : "
                << l_.undef_internal() << ")\n";
        }
        // YYBACKUP: push a token back onto the lookahead position.
        // Constraints (from Bison): action must be at end of rule and there
        // must be no current lookahead (yychar == YYEMPTY).  Violating either
        // is a runtime YYERROR.
        out << "#define YYBACKUP(Tok, Val)                                       \\\n";
        out << "    do                                                           \\\n";
        out << "        if (yychar == -2 && yylen == 1)                          \\\n";
        out << "        { yychar = (Tok); yylval = (Val);                        \\\n";
        out << "          yytoken = YYTRANSLATE(yychar);                         \\\n";
        out << "          YYPOPSTACK(1); goto yybackup; }                        \\\n";
        out << "        else { yyerror(\"syntax error: cannot back up\");        \\\n";
        out << "               YYERROR; }                                        \\\n";
        out << "    while (0)\n";
        out << "#define YYPOPSTACK(N) (yyssp -= (N), yyvsp -= (N)";
        if (g_.want_locations) out << ", yylsp -= (N)";
        out << ")\n";
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
        // First-fit packed displacement.  Each non-empty state's action row
        // (the sparse list of (col, action) pairs) is placed at a base
        // offset chosen so:
        //
        //   For every absolute index idx = base + col, c in 0..nT-1:
        //     claimed[idx] != c
        //
        // where claimed[] tracks "this idx already has an entry whose
        // column is c".  This stricter check (vs. only checking the
        // claim's columns against the new row's columns) prevents the
        // verbose-error walker / parser from getting false positives:
        // state s probing yyn=base[s]+T where s has no entry at T
        // necessarily lands on a cell with claimed[idx] != T, so the
        // "yycheck[idx] == T" test fails and the lookup falls through
        // to the default action.
        //
        // Goto rows pack into the same yytable using the same constraint
        // but with state index as the "column".  yypgoto + state_idx
        // looks up the goto target.
        const int NINF = -32768;
        int nT = l_.n_terminals();
        int nN = l_.n_nonterminals();
        int nS = l_.n_states();
        vector<int> yypact(nS, NINF), yypgoto(nN, NINF), yydefgoto(nN, 0);
        vector<int> yytable, yycheck;
        // claimed[idx] = col of the entry placed at idx, or -1 if free.
        // Parallel to yytable; grown as needed.
        vector<int> claimed;
        auto reserve = [&](int idx) {
            if ((int)claimed.size() <= idx) {
                claimed.resize(idx + 1, -1);
                yytable.resize(idx + 1, NINF);
                yycheck.resize(idx + 1, -1);
            }
        };
        // Encode an action.  Returns nullopt for a hole (no entry).
        auto encode_action = [&](int a) -> std::optional<int> {
            if (a == 0) return std::nullopt;
            if (a == LALR::ACCEPT) return 0; // never hit at runtime; YYFINAL covers accept
            if (a == LALR::ERR_MARK) return NINF;
            if (a > 0) return a - 1;     // shift dst
            return a;                     // reduce
        };

        struct Row {
            int row_id;     // state index for actions, nt index for gotos
            bool is_goto;
            int domain;     // nT for actions, nS for gotos
            vector<std::pair<int, int>> entries; // (col, action_or_goto)
            int default_goto = 0;        // for goto rows
        };
        vector<Row> rows;

        for (int s = 0; s < nS; s++) {
            Row r{s, false, nT, {}, 0};
            for (int t = 0; t < nT; t++) {
                int a = l_.action(s, t);
                if (a == 0) continue;
                r.entries.push_back({t, a});
            }
            if (!r.entries.empty()) rows.push_back(std::move(r));
        }
        for (int nt = 0; nt < nN; nt++) {
            Row r{nt, true, nS, {}, 0};
            std::map<int, int> freq;
            for (int s = 0; s < nS; s++) {
                int g = l_.goto_tab(s, nt);
                if (g != 0) freq[g]++;
            }
            int default_goto = 0, best = 0;
            for (auto& [g, n] : freq) if (n > best) { best = n; default_goto = g; }
            r.default_goto = default_goto;
            yydefgoto[nt] = (default_goto == 0) ? 0 : (default_goto - 1);
            for (int s = 0; s < nS; s++) {
                int g = l_.goto_tab(s, nt);
                if (g == 0 || g == default_goto) continue;
                r.entries.push_back({s, g});
            }
            if (!r.entries.empty()) rows.push_back(std::move(r));
        }
        // Pack widest rows first (col_span = last_col - first_col + 1),
        // breaking ties by descending entry count.  Wide+dense rows pin
        // the table's maximum column reach early, leaving narrow/sparse
        // rows to drop into the gaps without extending the span.  For
        // PG-class grammars this dramatically tightens the final layout.
        // Standard table-compression heuristic (Tarjan & Yao 1979 onwards).
        std::stable_sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) {
                int spa = a.entries.empty() ? 0
                    : (a.entries.back().first - a.entries.front().first + 1);
                int spb = b.entries.empty() ? 0
                    : (b.entries.back().first - b.entries.front().first + 1);
                if (spa != spb) return spa > spb;
                return a.entries.size() > b.entries.size();
            });

        // Strict packing for PG-class grammars.  Three accelerators:
        //
        // (1) Row dedup.  States whose (col, action) sets are identical can
        //     share a base; probing yytable[base+col] returns the right
        //     action for every state pointing there.  Hash by the byte
        //     serialization of the entry list.  This alone shaved ~50%
        //     off PG's table.
        //
        // (2) "Lowest-free" watermark.  After each placement, walk an
        //     index forward until it hits an unclaimed cell.  Subsequent
        //     rows start their base search at (lowest_free - first_col),
        //     skipping the dense head of the table that's already
        //     saturated.  Without this, every row re-scans from base 0.
        //
        // (3) Bitset for forbidden-base lookup (replaces unordered_set).
        //     A future row at the same base would land on the same cells
        //     and trigger false-positive yycheck matches, so each placed
        //     base is recorded.  The bitset is keyed by (base + bias)
        //     where bias covers the most-negative possible base.
        //
        // Two forbidden-base sets, one per row-type domain.  Action rows
        // (domain = nT) only see prior bases whose claims have any col
        // below nT; goto rows (domain = nS) see every prior base.
        std::vector<bool> forbidden_for_action;
        std::vector<bool> forbidden_for_goto;
        const int base_bias = std::max(nT, nS) + 4;
        auto forbid_query = [&](const std::vector<bool>& v, int base) {
            int i = base + base_bias;
            return i >= 0 && i < (int)v.size() && v[i];
        };
        auto forbid_mark = [&](std::vector<bool>& v, int base) {
            int i = base + base_bias;
            if (i < 0) return;
            if (i >= (int)v.size()) v.resize(i + 1, false);
            v[i] = true;
        };

        // Dedup map per row-type (action vs goto can't share even if
        // entries coincide -- different domains).
        std::unordered_map<std::string, int> action_dedup;
        std::unordered_map<std::string, int> goto_dedup;
        auto fingerprint = [](const Row& r) {
            std::string s;
            s.reserve(r.entries.size() * 8);
            for (auto& e : r.entries) {
                s.append(reinterpret_cast<const char*>(&e.first),  sizeof(e.first));
                s.append(reinterpret_cast<const char*>(&e.second), sizeof(e.second));
            }
            return s;
        };

        // Smallest claimed[] index that is currently unclaimed.  Maintained
        // incrementally: after each placement we walk forward past any
        // cells that just became claimed.
        int lowest_free = 0;
        auto walk_lowest_free = [&]() {
            while (lowest_free < (int)claimed.size() &&
                   claimed[lowest_free] != -1)
                lowest_free++;
        };

        for (const Row& r : rows) {
            // Dedup: identical entry list -> reuse the existing base.
            std::string fp = fingerprint(r);
            auto& dedup_map = r.is_goto ? goto_dedup : action_dedup;
            auto it = dedup_map.find(fp);
            if (it != dedup_map.end()) {
                int base = it->second;
                if (r.is_goto) yypgoto[r.row_id] = base;
                else           yypact[r.row_id]  = base;
                continue;
            }

            // Search for a base.  Entries are col-sorted, so the smallest
            // entry idx after placement will be (base + first_col); we
            // start with that landing on lowest_free.
            const int first_col = r.entries.front().first;
            int base = lowest_free - first_col;
            const auto& my_forbidden = r.is_goto ? forbidden_for_goto
                                                 : forbidden_for_action;
            for (;;) {
                if (forbid_query(my_forbidden, base)) { base++; continue; }
                bool ok = true;
                for (auto& e : r.entries) {
                    int idx = base + e.first;
                    if (idx < 0) { ok = false; break; }
                    if (idx < (int)claimed.size() && claimed[idx] != -1) {
                        ok = false; break;
                    }
                }
                if (ok) break;
                base++;
            }

            // Place.
            bool has_small_col = false;
            for (auto& e : r.entries) {
                int idx = base + e.first;
                reserve(idx);
                claimed[idx] = e.first;
                if (e.first < nT) has_small_col = true;
                if (r.is_goto) {
                    yytable[idx] = e.second - 1;
                    yycheck[idx] = e.first;
                } else {
                    auto enc = encode_action(e.second);
                    yytable[idx] = enc ? *enc : NINF;
                    yycheck[idx] = e.first;
                }
            }

            forbid_mark(forbidden_for_goto, base);
            if (has_small_col) forbid_mark(forbidden_for_action, base);
            dedup_map.emplace(std::move(fp), base);
            if (r.is_goto) yypgoto[r.row_id] = base;
            else            yypact[r.row_id]  = base;

            walk_lowest_free();
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
            string nm;
            if (s == g_.eof_sym) nm = "end of file";
            else if (s == g_.error_sym) nm = "error";
            else if (s == g_.undef_sym) nm = "invalid token";
            else {
                // Prefer a string-literal alias ("if" instead of IF) for
                // diagnostic output, matching Bison's yytname convention.
                // The alias is stored with surrounding quotes; strip them
                // so error messages read 'expecting if' not 'expecting "if"'.
                nm = g_.syms[s].name;
                for (const auto& sym : g_.syms) {
                    if (sym.alias_of == s && sym.name.size() >= 2 &&
                        sym.name.front() == '"' && sym.name.back() == '"') {
                        nm = sym.name.substr(1, sym.name.size() - 2);
                        break;
                    }
                }
            }
            out << "  \"" << esc(nm) << "\",\n";
        }
        out << "  0\n};\n";
    }

    void emit_yyerror_default(Buf out) {
        // Bison doesn't emit a default yyerror — the user must supply one
        // (or link liby/-ly).  We follow the same convention so a grammar
        // that defines its own static yyerror() in its prologue (like
        // gettext's intl/plural.y) doesn't conflict with a generator-
        // emitted weak symbol.  yyerror is declared in emit_driver above
        // for the call sites; the actual body is the user's responsibility.
        (void)out;
        // Verbose / detailed error: build a message naming the unexpected
        // token and the set of tokens that would be acceptable in this state,
        // walking yypact[]/yytable[]/yycheck[].
        if (g_.parse_error_mode == "verbose" || g_.parse_error_mode == "detailed") {
            // LAC simulator: returns 1 if 'yyx' would shift or accept after
            // following the chain of default/no-context reductions starting
            // from 'yystate', given the current state stack.  Returns 0 if
            // it would error.  Operates on a scratch copy of the top of the
            // stack so the real parser is not perturbed.
            if (g_.parse_lac == "full") {
                out << "static int yy_lac(const short *yyss, const short *yyssp,\n";
                out << "                  int yystate, int yyx) {\n";
                out << "    /* Copy enough of the state stack into a scratch buffer that\n";
                out << "     * reductions can pop and goto without touching the real one. */\n";
                out << "    short scratch[64];\n";
                out << "    int sp = 0;\n";
                out << "    long depth = yyssp - yyss;\n";
                out << "    if (depth >= (long)(sizeof(scratch)/sizeof(scratch[0])) - 4)\n";
                out << "        return 1; /* conservative: claim acceptable */\n";
                out << "    for (long i = 0; i <= depth; i++) scratch[sp++] = yyss[i];\n";
                out << "    int s = yystate;\n";
                out << "    for (;;) {\n";
                out << "        int yyn = yypact[s];\n";
                out << "        if (!yypact_value_is_default(yyn)) {\n";
                out << "            int yychk = yyn + yyx;\n";
                out << "            if (yychk >= 0 && yychk < (int)YYTABLE_SIZE\n";
                out << "                && yycheck[yychk] == yyx\n";
                out << "                && !yytable_value_is_error(yytable[yychk])) {\n";
                out << "                int act = yytable[yychk];\n";
                out << "                if (act >= 0) return 1;        /* shift / accept */\n";
                out << "                yyn = -act;                    /* reduce by yyn */\n";
                out << "            } else { goto try_default; }\n";
                out << "        } else {\n";
                out << "          try_default:\n";
                out << "            yyn = yydefact[s];\n";
                out << "            if (yyn == 0) return 0;            /* error */\n";
                out << "        }\n";
                out << "        /* reduce by rule yyn */\n";
                out << "        int len = yyr2[yyn];\n";
                out << "        int lhs = yyr1[yyn] - YYNTOKENS;\n";
                out << "        if (sp - len < 1) return 1; /* shouldn't happen */\n";
                out << "        sp -= len;\n";
                out << "        int gpos = yypgoto[lhs] + scratch[sp - 1];\n";
                out << "        if (gpos >= 0 && gpos < (int)YYTABLE_SIZE && yycheck[gpos] == scratch[sp - 1])\n";
                out << "            s = yytable[gpos];\n";
                out << "        else\n";
                out << "            s = yydefgoto[lhs];\n";
                out << "        if (sp >= (int)(sizeof(scratch)/sizeof(scratch[0]))) return 1;\n";
                out << "        scratch[sp++] = (short)s;\n";
                out << "    }\n";
                out << "}\n";
            }
            out << "static void yysyntax_error(int yystate, int yytoken";
            if (g_.parse_lac == "full")
                out << ", const short *yyss, const short *yyssp";
            if (pure() && g_.want_locations) out << ", YYLTYPE *yyllocp";
            for (auto& p : g_.parse_params) out << ", " << p;
            out << ") {\n";
            out << "    char buf[512];\n";
            out << "    size_t off = 0;\n";
            out << "    int n = snprintf(buf + off, sizeof(buf) - off, \"syntax error\");\n";
            out << "    if (n > 0) off += (size_t)n;\n";
            out << "    if (yytoken >= 0 && yytoken < YYNTOKENS) {\n";
            out << "        n = snprintf(buf + off, sizeof(buf) - off, \", unexpected %s\", yytname[yytoken]);\n";
            out << "        if (n > 0) off += (size_t)n;\n";
            out << "    }\n";
            // Up to 4 expected tokens.
            out << "    int yyx;\n";
            out << "    int yyn = yypact[yystate];\n";
            out << "    int count = 0;\n";
            out << "    int expected[4];\n";
            out << "    if (!yypact_value_is_default(yyn)) {\n";
            out << "        for (yyx = 0; yyx < YYNTOKENS && count < 4; ++yyx) {\n";
            out << "            int yychk = yyn + yyx;\n";
            out << "            if (yychk >= 0 && yychk < (int)YYTABLE_SIZE\n";
            out << "                && yycheck[yychk] == yyx\n";
            out << "                && !yytable_value_is_error(yytable[yychk])\n";
            out << "                && yyx != " << l_.error_internal() << "\n";
            out << "                && yyx != " << l_.undef_internal() << ") {\n";
            if (g_.parse_lac == "full") {
                out << "                if (!yy_lac(yyss, yyssp, yystate, yyx)) continue;\n";
            }
            out << "                expected[count++] = yyx;\n";
            out << "            }\n";
            out << "        }\n";
            out << "    }\n";
            out << "    for (int i = 0; i < count; ++i) {\n";
            out << "        const char *prefix = (i == 0) ? \", expecting \" :\n";
            out << "                             (i + 1 == count) ? \" or \" : \", \";\n";
            out << "        n = snprintf(buf + off, sizeof(buf) - off, \"%s%s\", prefix, yytname[expected[i]]);\n";
            out << "        if (n > 0) off += (size_t)n;\n";
            out << "        if (off >= sizeof(buf)) { off = sizeof(buf) - 1; break; }\n";
            out << "    }\n";
            out << "    buf[off < sizeof(buf) ? off : sizeof(buf) - 1] = 0;\n";
            out << "    yyerror(";
            if (pure() && g_.want_locations) out << "yyllocp, ";
            out << params_call_no_leading_comma(g_.parse_params);
            if (!g_.parse_params.empty()) out << ", ";
            out << "buf);\n";
            out << "}\n";
        }
        // Custom parse-error mode: define the yypcontext_t struct and
        // the helper functions.  Non-static so user code can call them.
        // Forward typedefs are also re-emitted here in case the user's
        // source doesn't #include the generated .h.
        if (g_.parse_error_mode == "custom") {
            out << "typedef int yysymbol_kind_t;\n";
            out << "typedef struct yypcontext_s yypcontext_t;\n";
            out << "struct yypcontext_s {\n";
            out << "    int yystate;\n";
            out << "    int yytoken;\n";
            if (g_.want_locations) out << "    YYLTYPE *yylocp;\n";
            out << "};\n";
            out << "yysymbol_kind_t yypcontext_token(const yypcontext_t *yyctx) {\n";
            out << "    return yyctx->yytoken;\n";
            out << "}\n";
            if (g_.want_locations) {
                out << "const YYLTYPE *yypcontext_location(const yypcontext_t *yyctx) {\n";
                out << "    return yyctx->yylocp;\n";
                out << "}\n";
            }
            out << "int yypcontext_expected_tokens(const yypcontext_t *yyctx,\n";
            out << "        yysymbol_kind_t *yyarg, int yyargn) {\n";
            out << "    int yyn = yypact[yyctx->yystate];\n";
            out << "    int count = 0;\n";
            out << "    if (!yypact_value_is_default(yyn)) {\n";
            out << "        for (int yyx = 0; yyx < YYNTOKENS && count < yyargn; ++yyx) {\n";
            out << "            int yychk = yyn + yyx;\n";
            out << "            if (yychk >= 0 && yychk < (int)YYTABLE_SIZE\n";
            out << "                && yycheck[yychk] == yyx\n";
            out << "                && !yytable_value_is_error(yytable[yychk])\n";
            out << "                && yyx != " << l_.error_internal() << "\n";
            out << "                && yyx != " << l_.undef_internal() << ") {\n";
            out << "                yyarg[count++] = yyx;\n";
            out << "            }\n";
            out << "        }\n";
            out << "    }\n";
            out << "    return count;\n";
            out << "}\n";
            out << "const char *yysymbol_name(yysymbol_kind_t k) {\n";
            out << "    if (k >= 0 && k < YYNTOKENS + YYNNTS) return yytname[k];\n";
            out << "    return \"?\";\n";
            out << "}\n";
        }
    }

    // Build the body of a yydestruct/yysymbol_print dispatcher: a switch on
    // internal symbol kind that runs the matching user code body.  When no
    // sym-specific body is registered, fall back to a tag-keyed default
    // (<tag> for typed, <*> for any-typed, <> for untyped).
    string dispatcher_body(const std::map<int, string>& by_sym,
                            const std::map<string, string>& by_tag) const {
        string out;
        out += "    switch (yykind) {\n";
        for (int internal = 0; internal < l_.n_total_syms(); internal++) {
            int sym = l_.internal_to_sym(internal);
            string body;
            auto it = by_sym.find(sym);
            if (it != by_sym.end()) body = it->second;
            else {
                const string& tag = g_.syms[sym].type_tag;
                if (!tag.empty()) {
                    auto t = by_tag.find(tag);
                    if (t == by_tag.end()) t = by_tag.find("*");
                    if (t == by_tag.end()) t = by_tag.find("<*>");
                    if (t != by_tag.end()) body = t->second;
                } else {
                    auto t = by_tag.find("");
                    if (t == by_tag.end()) t = by_tag.find("<>");
                    if (t != by_tag.end()) body = t->second;
                }
            }
            if (body.empty()) continue;
            out += "    case "; out += std::to_string(internal); out += ":\n";
            out += "      { "; out += body; out += " }\n";
            out += "      break;\n";
        }
        out += "    default: break;\n";
        out += "    }\n";
        return out;
    }

    bool any_destructor() const {
        return !g_.destructor_by_sym.empty() || !g_.destructor_default.empty();
    }
    bool any_printer() const {
        return !g_.printer_by_sym.empty() || !g_.printer_default.empty();
    }

    void emit_destructor(Buf out) {
        if (!any_destructor()) return;
        out << "static void yydestruct(const char *yymsg, int yykind, "
               "YYSTYPE *yyvaluep";
        if (g_.want_locations) out << ", YYLTYPE *yylocationp";
        for (auto& p : g_.parse_params) out << ", " << p;
        out << ") {\n";
        out << "    (void)yymsg; (void)yyvaluep;\n";
        if (g_.want_locations) out << "    (void)yylocationp;\n";
        for (auto& p : g_.parse_params) {
            size_t end = p.find_last_not_of(" \t\r\n");
            if (end == string::npos) continue;
            size_t start = end;
            while (start > 0 && (ch_isalnum((unsigned char)p[start - 1]) || p[start - 1] == '_'))
                start--;
            out << "    (void)" << p.substr(start, end - start + 1) << ";\n";
        }
        // The user's body may use $$ and @$; rewrite those to (*yyvaluep)
        // and (*yylocationp).  Translate via a synthetic Production whose
        // tag context comes from the symbol when emitted.
        out << "#define yyx_value(t) ((*yyvaluep)" << ".t)\n";
        out << "    switch (yykind) {\n";
        for (int internal = 0; internal < l_.n_total_syms(); internal++) {
            int sym = l_.internal_to_sym(internal);
            string body;
            auto it = g_.destructor_by_sym.find(sym);
            if (it != g_.destructor_by_sym.end()) body = it->second;
            else {
                const string& tag = g_.syms[sym].type_tag;
                if (!tag.empty()) {
                    auto t = g_.destructor_default.find(tag);
                    if (t == g_.destructor_default.end()) t = g_.destructor_default.find("*");
                    if (t != g_.destructor_default.end()) body = t->second;
                } else {
                    auto t = g_.destructor_default.find("");
                    if (t != g_.destructor_default.end()) body = t->second;
                }
            }
            if (body.empty()) continue;
            // Translate $$ to (*yyvaluep) (with tag if known) and @$ to (*yylocationp).
            string translated = translate_destructor_body(body, g_.syms[sym].type_tag);
            out << "    case " << internal << ":\n";
            out << "      { " << translated << " }\n";
            out << "      break;\n";
        }
        out << "    default: break;\n";
        out << "    }\n";
        out << "#undef yyx_value\n";
        out << "}\n";
    }

    void emit_printer(Buf out) {
        if (!any_printer()) return;
        out << "static void yysymbol_print(FILE *yyo, int yykind, "
               "YYSTYPE *yyvaluep";
        if (g_.want_locations) out << ", YYLTYPE *yylocationp";
        out << ") {\n";
        out << "    (void)yyvaluep;\n";
        if (g_.want_locations) out << "    (void)yylocationp;\n";
        out << "    switch (yykind) {\n";
        for (int internal = 0; internal < l_.n_total_syms(); internal++) {
            int sym = l_.internal_to_sym(internal);
            string body;
            auto it = g_.printer_by_sym.find(sym);
            if (it != g_.printer_by_sym.end()) body = it->second;
            else {
                const string& tag = g_.syms[sym].type_tag;
                if (!tag.empty()) {
                    auto t = g_.printer_default.find(tag);
                    if (t == g_.printer_default.end()) t = g_.printer_default.find("*");
                    if (t != g_.printer_default.end()) body = t->second;
                } else {
                    auto t = g_.printer_default.find("");
                    if (t != g_.printer_default.end()) body = t->second;
                }
            }
            if (body.empty()) continue;
            string translated = translate_destructor_body(body, g_.syms[sym].type_tag);
            out << "    case " << internal << ":\n";
            out << "      { " << translated << " }\n";
            out << "      break;\n";
        }
        out << "    default: break;\n";
        out << "    }\n";
        out << "}\n";
    }

    // Tiny version of translate_action specialized for destructor/printer
    // bodies, where the only parser-generator references are $$ and @$.
    string translate_destructor_body(const string& s, const string& tag) {
        string out;
        size_t i = 0;
        while (i < s.size()) {
            char c = s[i];
            if (c == '"' || c == '\'') {
                char quote = c;
                out += c; i++;
                while (i < s.size()) {
                    if (s[i] == '\\' && i + 1 < s.size()) {
                        out += s[i++]; out += s[i++]; continue;
                    }
                    out += s[i];
                    if (s[i] == quote) { i++; break; }
                    i++;
                }
                continue;
            }
            if (c == '$' && i + 1 < s.size() && s[i + 1] == '$') {
                if (g_.has_union && !tag.empty()) {
                    out += "((*yyvaluep)."; out += tag; out += ")";
                } else {
                    out += "(*yyvaluep)";
                }
                i += 2; continue;
            }
            if (c == '@' && g_.want_locations && i + 1 < s.size() && s[i + 1] == '$') {
                out += "(*yylocationp)";
                i += 2; continue;
            }
            out += c; i++;
        }
        return out;
    }

    void emit_trace_macros(Buf out) {
        if (!g_.parse_trace) return;
        out << "#ifndef YYDEBUG\n# define YYDEBUG 1\n#endif\n";
        out << "#if YYDEBUG\n";
        out << "# define YYDPRINTF(Args) do { if (yydebug) fprintf Args; } while (0)\n";
        out << "# define YY_SYMBOL_PRINT(Title, Kind, Val, Loc) \\\n";
        out << "    do { if (yydebug) { fprintf(stderr, \"%s \", Title); ";
        if (any_printer())
            out << "yysymbol_print(stderr, Kind, Val" << (g_.want_locations ? ", Loc" : "") << "); ";
        else
            out << "(void)(Kind); (void)(Val); ";
        out << "fprintf(stderr, \"\\n\"); } } while (0)\n";
        out << "# define YY_REDUCE_PRINT(Rule) \\\n";
        out << "    do { if (yydebug) fprintf(stderr, \"Reducing stack by rule %d\\n\", Rule); } while (0)\n";
        out << "# define YY_STACK_PRINT(B, T) do {} while (0)\n";
        out << "#else\n";
        out << "# define YYDPRINTF(Args) do {} while (0)\n";
        out << "# define YY_SYMBOL_PRINT(T, K, V, L) do {} while (0)\n";
        out << "# define YY_REDUCE_PRINT(R) do {} while (0)\n";
        out << "# define YY_STACK_PRINT(B, T) do {} while (0)\n";
        out << "#endif\n";
    }

    void emit_driver(Buf out) {
        const bool L = g_.want_locations;
        const bool P = pure();
        // yylex prototype — pure parsers receive YYSTYPE*[, YYLTYPE*][, lex-params...].
        out << "extern int yylex(";
        if (P) {
            out << "YYSTYPE *yylvalp";
            if (L) out << ", YYLTYPE *yyllocp";
        } else {
            out << "void";
        }
        for (auto& p : g_.lex_params) out << ", " << p;
        out << ");\n";
        // yyerror prototype — pure parsers receive (YYLTYPE*[, parse-params...], const char*).
        out << "void yyerror(";
        if (P && L) out << "YYLTYPE *yyllocp, ";
        for (auto& p : g_.parse_params) out << p << ", ";
        out << "const char *msg);\n";
        // YYTRANSLATE was emitted by emit_constants above.
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
        out << "int yyparse(" << params_decl_signature() << ") {\n";
        if (P) {
            out << "    YYSTYPE yylval;\n";
            out << "    int yychar = -2;\n";
            out << "    int yynerrs = 0;\n";
            if (L) out << "    YYLTYPE yylloc;\n";
        }
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
        if (!g_.initial_action.empty()) {
            // Run user's %initial-action.  $$ and @$ aren't really meaningful
            // here, so we emit it verbatim (consistent with bison's behavior
            // — references resolve to the initial yyval/yyloc).
            out << "    {\n";
            if (!opts_.no_lines) out << "#line 1 \"" << g_.source_file << "\"\n";
            out << g_.initial_action << "\n    }\n";
        }
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
        if (g_.parse_trace) out << "    if (yychar == -2) YYDPRINTF((stderr, \"Reading a token\\n\"));\n";
        out << "    if (yychar == -2) yychar = yylex(" << yylex_call_args() << ");\n";
        if (g_.parse_trace) {
            out << "    if (yychar <= 0)\n";
            out << "        YYDPRINTF((stderr, \"Now at end of input.\\n\"));\n";
            out << "    else\n";
            out << "        YY_SYMBOL_PRINT(\"Next token is\", YYTRANSLATE(yychar), &yylval, "
                << (L ? "&yylloc" : "(void*)0") << ");\n";
        }
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
        if (g_.parse_trace) {
            out << "    YY_SYMBOL_PRINT(\"Shifting\", yytoken, &yylval, "
                << (L ? "&yylloc" : "(void*)0") << ");\n";
        }
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
        if (g_.parse_trace)
            out << "    YY_REDUCE_PRINT(yyn);\n";
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
        const bool verbose = (g_.parse_error_mode == "verbose" ||
                              g_.parse_error_mode == "detailed");
        const bool custom = (g_.parse_error_mode == "custom");
        if (verbose) {
            out << "    if (!yyerrstatus) { ++yynerrs; yysyntax_error(yystate, yytoken"
                << yysyntax_error_extra_args() << "); }\n";
        } else if (custom) {
            out << "    if (!yyerrstatus) {\n";
            out << "        ++yynerrs;\n";
            out << "        yypcontext_t yyctx;\n";
            out << "        yyctx.yystate = yystate;\n";
            out << "        yyctx.yytoken = yytoken;\n";
            if (g_.want_locations) out << "        yyctx.yylocp = &yylloc;\n";
            out << "        yyreport_syntax_error(&yyctx";
            for (auto& p : g_.parse_params) {
                size_t end = p.find_last_not_of(" \t\r\n");
                if (end == string::npos) continue;
                size_t start = end;
                while (start > 0 && (ch_isalnum((unsigned char)p[start - 1]) || p[start - 1] == '_'))
                    start--;
                out << ", " << p.substr(start, end - start + 1);
            }
            out << ");\n";
            out << "    }\n";
        } else {
            out << "    if (!yyerrstatus) { ++yynerrs; yyerror("
                << yyerror_call_args("\"syntax error\"") << "); }\n";
        }
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
        if (any_destructor()) {
            out << "        yydestruct(\"Error: popping\", yystos[*yyssp], yyvsp";
            if (L) out << ", yylsp";
            out << params_call(g_.parse_params) << ");\n";
        }
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
        out << "    yyerror(" << yyerror_call_args("\"memory exhausted\"") << ");\n";
        out << "    yyresult = 2; goto yyreturn;\n";
        out << "yyreturn:\n";
        out << "    if (yyss != yyssa) { free(yyss); free(yyvs);" << (L ? " free(yyls);" : "") << " }\n";
        out << "    return yyresult;\n";
        out << "}\n";
    }

    // Push-parser API: yypstate, yypstate_new, yypush_parse, yypstate_delete.
    // Implements the same state machine as the pull driver above, but the
    // entire state lives in a heap-allocated yypstate so that yypush_parse
    // can return YYPUSH_MORE when it needs another token, and resume on the
    // next call.  When api.push-pull=both, this lives alongside yyparse;
    // when push-only, this replaces it.
    void emit_push_driver(Buf out) {
        const bool L = g_.want_locations;
        const bool push_both = (g_.api_push_pull == "both");
        out << "#define YYPUSH_MORE 4\n";
        out << "typedef struct yypstate yypstate;\n";
        out << "struct yypstate {\n";
        out << "    int yynew;\n";
        out << "    int yystate;\n";
        out << "    int yyerrstatus;\n";
        out << "    int yystacksize;\n";
        out << "    short *yyss;\n";
        out << "    YYSTYPE *yyvs;\n";
        if (L) out << "    YYLTYPE *yyls;\n";
        out << "    short yyssa[YYINITDEPTH];\n";
        out << "    YYSTYPE yyvsa[YYINITDEPTH];\n";
        if (L) out << "    YYLTYPE yylsa[YYINITDEPTH];\n";
        out << "    short *yyssp;\n";
        out << "    YYSTYPE *yyvsp;\n";
        if (L) out << "    YYLTYPE *yylsp;\n";
        out << "    int yychar;\n";
        out << "    YYSTYPE yylval;\n";
        if (L) out << "    YYLTYPE yylloc;\n";
        out << "    int yynerrs;\n";
        out << "};\n";

        // Constructor.
        out << "yypstate *yypstate_new(void) {\n";
        out << "    yypstate *yyps = (yypstate*)malloc(sizeof(*yyps));\n";
        out << "    if (!yyps) return NULL;\n";
        out << "    yyps->yynew = 1;\n";
        out << "    yyps->yychar = -2;\n";
        out << "    yyps->yyss = NULL;\n";
        out << "    yyps->yyvs = NULL;\n";
        if (L) out << "    yyps->yyls = NULL;\n";
        out << "    return yyps;\n";
        out << "}\n";

        // Destructor.
        out << "void yypstate_delete(yypstate *yyps) {\n";
        out << "    if (!yyps) return;\n";
        out << "    if (yyps->yyss && yyps->yyss != yyps->yyssa) {\n";
        out << "        free(yyps->yyss); free(yyps->yyvs);";
        if (L) out << " free(yyps->yyls);";
        out << "\n    }\n";
        out << "    free(yyps);\n";
        out << "}\n";

        // The push-parse function.
        out << "int yypush_parse(yypstate *yyps, int yypushed_char, "
               "YYSTYPE const *yypushed_val";
        if (L) out << ", YYLTYPE const *yypushed_loc";
        for (auto& p : g_.parse_params) out << ", " << p;
        out << ") {\n";

        // Same locals as pull but synced into yyps. Using ps->* directly
        // throughout keeps state across calls.
        out << "    int yyn;\n";
        out << "    int yyresult;\n";
        out << "    int yytoken = -2;\n";
        out << "    YYSTYPE yyval;\n";
        if (L) out << "    YYLTYPE yyloc;\n";
        out << "    int yylen = 0;\n";
        out << "\n";
        out << "    if (yyps->yynew) {\n";
        out << "        yyps->yynew = 0;\n";
        out << "        yyps->yystate = 0;\n";
        out << "        yyps->yyerrstatus = 0;\n";
        out << "        yyps->yystacksize = YYINITDEPTH;\n";
        out << "        yyps->yyss = yyps->yyssa;\n";
        out << "        yyps->yyvs = yyps->yyvsa;\n";
        if (L) out << "        yyps->yyls = yyps->yylsa;\n";
        out << "        yyps->yyssp = yyps->yyss;\n";
        out << "        yyps->yyvsp = yyps->yyvs;\n";
        if (L) out << "        yyps->yylsp = yyps->yyls;\n";
        out << "        *yyps->yyssp = 0;\n";
        out << "        yyps->yychar = -2;\n";
        out << "        yyps->yynerrs = 0;\n";
        // After init, also accept the caller's first token.  Bison treats
        // the first yypush_parse call as an init+feed: the first token
        // must arrive in time to be matched after the start-state reduces.
        out << "        yyps->yychar = yypushed_char;\n";
        out << "        if (yypushed_val) yyps->yylval = *yypushed_val;\n";
        if (L) out << "        if (yypushed_loc) yyps->yylloc = *yypushed_loc;\n";
        out << "        goto yysetstate;\n";
        out << "    }\n";
        // Resume: caller has provided a token.
        out << "    yyps->yychar = yypushed_char;\n";
        out << "    if (yypushed_val) yyps->yylval = *yypushed_val;\n";
        if (L) out << "    if (yypushed_loc) yyps->yylloc = *yypushed_loc;\n";
        out << "    goto yybackup;\n";
        out << "\n";

        // The body: same logic as pull driver, but using yyps->* and
        // returning YYPUSH_MORE when a token is needed.
        out << "yynewstate:\n";
        out << "    yyps->yyssp++;\n";
        out << "yysetstate:\n";
        out << "    *yyps->yyssp = (short)yyps->yystate;\n";
        out << "    if (yyps->yyss + yyps->yystacksize - 1 <= yyps->yyssp) {\n";
        out << "        long yysize = yyps->yyssp - yyps->yyss + 1;\n";
        out << "        if (yyps->yystacksize >= YYMAXDEPTH) goto yyexhaustedlab;\n";
        out << "        yyps->yystacksize *= 2;\n";
        out << "        if (yyps->yystacksize > YYMAXDEPTH) yyps->yystacksize = YYMAXDEPTH;\n";
        out << "        short *new_ss = (short*)malloc((size_t)yyps->yystacksize * sizeof(short));\n";
        out << "        YYSTYPE *new_vs = (YYSTYPE*)malloc((size_t)yyps->yystacksize * sizeof(YYSTYPE));\n";
        if (L) out << "        YYLTYPE *new_ls = (YYLTYPE*)malloc((size_t)yyps->yystacksize * sizeof(YYLTYPE));\n";
        out << "        if (!new_ss || !new_vs" << (L ? " || !new_ls" : "")
            << ") { free(new_ss); free(new_vs);" << (L ? " free(new_ls);" : "") << " goto yyexhaustedlab; }\n";
        out << "        memcpy(new_ss, yyps->yyss, (size_t)yysize * sizeof(short));\n";
        out << "        memcpy(new_vs, yyps->yyvs, (size_t)yysize * sizeof(YYSTYPE));\n";
        if (L) out << "        memcpy(new_ls, yyps->yyls, (size_t)yysize * sizeof(YYLTYPE));\n";
        out << "        if (yyps->yyss != yyps->yyssa) { free(yyps->yyss); free(yyps->yyvs);";
        if (L) out << " free(yyps->yyls);";
        out << " }\n";
        out << "        yyps->yyss = new_ss; yyps->yyvs = new_vs;";
        if (L) out << " yyps->yyls = new_ls;";
        out << "\n";
        out << "        yyps->yyssp = yyps->yyss + yysize - 1;\n";
        out << "        yyps->yyvsp = yyps->yyvs + yysize - 1;\n";
        if (L) out << "        yyps->yylsp = yyps->yyls + yysize - 1;\n";
        out << "    }\n";
        out << "    if (yyps->yystate == YYFINAL) goto yyacceptlab;\n";
        out << "\n";
        out << "yybackup:\n";
        out << "    yyn = yypact[yyps->yystate];\n";
        out << "    if (yypact_value_is_default(yyn)) goto yydefault;\n";
        // The push pause point.
        out << "    if (yyps->yychar == -2) return YYPUSH_MORE;\n";
        out << "    if (yyps->yychar <= 0) { yyps->yychar = 0; yytoken = 0; }\n";
        out << "    else if (yyps->yychar == 256) { yyps->yychar = 257; yytoken = YYTRANSLATE(257); goto yyerrlab1; }\n";
        out << "    else yytoken = YYTRANSLATE(yyps->yychar);\n";
        out << "    yyn += yytoken;\n";
        out << "    if (yyn < 0 || yyn >= (int)YYTABLE_SIZE || yycheck[yyn] != yytoken) goto yydefault;\n";
        out << "    yyn = yytable[yyn];\n";
        out << "    if (yyn <= 0) {\n";
        out << "        if (yytable_value_is_error(yyn)) goto yyerrlab;\n";
        out << "        if (yyn == 0) goto yyacceptlab;\n";
        out << "        yyn = -yyn;\n";
        out << "        goto yyreduce;\n";
        out << "    }\n";
        out << "    if (yyps->yyerrstatus) yyps->yyerrstatus--;\n";
        out << "    yyps->yystate = yyn;\n";
        out << "    *++yyps->yyvsp = yyps->yylval;\n";
        if (L) out << "    *++yyps->yylsp = yyps->yylloc;\n";
        out << "    yyps->yychar = -2;\n";
        out << "    goto yynewstate;\n";
        out << "\n";
        out << "yydefault:\n";
        out << "    yyn = yydefact[yyps->yystate];\n";
        out << "    if (yyn == 0) goto yyerrlab;\n";
        out << "    goto yyreduce;\n";
        out << "\n";
        out << "yyreduce:\n";
        out << "    yylen = yyr2[yyn];\n";
        out << "    if (yylen) yyval = yyps->yyvsp[1 - yylen];\n";
        out << "    else memset(&yyval, 0, sizeof(yyval));\n";
        if (L) out << "    YYLLOC_DEFAULT(yyloc, (yyps->yylsp - yylen), yylen);\n";
        // Action switch.  Inside actions, $$/$N reference yyval/yyvsp[*]
        // — we need yyvsp to point into ps->yyvsp.  Use macro shim.
        out << "#define yyvsp (yyps->yyvsp)\n";
        if (L) out << "#define yylsp (yyps->yylsp)\n";
        out << "    switch (yyn) {\n";
        for (int i = 1; i < l_.n_rules(); i++) {
            const Production& p = l_.prod(i);
            if (p.action.empty()) continue;
            out << "    case " << i << ":\n";
            out << "      { " << translate_action(p) << " }\n";
            out << "      break;\n";
        }
        out << "    }\n";
        out << "#undef yyvsp\n";
        if (L) out << "#undef yylsp\n";
        out << "    yyps->yyssp -= yylen;\n";
        out << "    yyps->yyvsp -= yylen;\n";
        if (L) out << "    yyps->yylsp -= yylen;\n";
        out << "    yylen = 0;\n";
        out << "    *++yyps->yyvsp = yyval;\n";
        if (L) out << "    *++yyps->yylsp = yyloc;\n";
        out << "    {\n";
        out << "        int yylhs_internal = yyr1[yyn];\n";
        out << "        int nt = yylhs_internal - YYNTOKENS;\n";
        out << "        int gpos = yypgoto[nt] + *yyps->yyssp;\n";
        out << "        if (gpos >= 0 && gpos < (int)YYTABLE_SIZE && yycheck[gpos] == *yyps->yyssp)\n";
        out << "            yyps->yystate = yytable[gpos];\n";
        out << "        else\n";
        out << "            yyps->yystate = yydefgoto[nt];\n";
        out << "    }\n";
        out << "    goto yynewstate;\n";
        out << "\n";
        out << "yyerrlab:\n";
        const bool verbose = (g_.parse_error_mode == "verbose" ||
                              g_.parse_error_mode == "detailed");
        if (verbose) {
            out << "    if (!yyps->yyerrstatus) { ++yyps->yynerrs; "
                << "yysyntax_error(yyps->yystate, yytoken"
                << yysyntax_error_extra_args(/*push=*/true) << "); }\n";
        } else {
            out << "    if (!yyps->yyerrstatus) { ++yyps->yynerrs; yyerror(";
            if (pure() && L) out << "&yyps->yylloc, ";
            for (auto& p : g_.parse_params) {
                size_t end = p.find_last_not_of(" \t\r\n");
                if (end == string::npos) continue;
                size_t start = end;
                while (start > 0 && (ch_isalnum((unsigned char)p[start - 1]) || p[start - 1] == '_'))
                    start--;
                out << p.substr(start, end - start + 1) << ", ";
            }
            out << "\"syntax error\"); }\n";
        }
        out << "    if (yyps->yyerrstatus == 3) {\n";
        out << "        if (yyps->yychar <= 0) goto yyabortlab;\n";
        out << "        yyps->yychar = -2;\n";
        out << "    }\n";
        out << "yyerrlab1:\n";
        out << "    yyps->yyerrstatus = 3;\n";
        out << "    for (;;) {\n";
        out << "        yyn = yypact[yyps->yystate];\n";
        out << "        if (!yypact_value_is_default(yyn)) {\n";
        out << "            int err_internal = " << l_.error_internal() << ";\n";
        out << "            int idx = yyn + err_internal;\n";
        out << "            if (idx >= 0 && idx < (int)YYTABLE_SIZE && yycheck[idx] == err_internal) {\n";
        out << "                yyn = yytable[idx];\n";
        out << "                if (yyn > 0) {\n";
        out << "                    yyps->yystate = yyn;\n";
        out << "                    *++yyps->yyvsp = yyps->yylval;\n";
        if (L) out << "                    *++yyps->yylsp = yyps->yylloc;\n";
        out << "                    goto yynewstate;\n";
        out << "                }\n";
        out << "            }\n";
        out << "        }\n";
        out << "        if (yyps->yyssp == yyps->yyss) goto yyabortlab;\n";
        out << "        yyps->yyvsp--;\n";
        if (L) out << "        yyps->yylsp--;\n";
        out << "        yyps->yystate = *--yyps->yyssp;\n";
        out << "    }\n";
        out << "\n";
        out << "yyacceptlab:\n";
        out << "    yyresult = 0; goto yyreturn;\n";
        out << "yyabortlab:\n";
        out << "    yyresult = 1; goto yyreturn;\n";
        out << "yyexhaustedlab:\n";
        out << "    yyresult = 2; goto yyreturn;\n";
        out << "yyreturn:\n";
        out << "    yyps->yynew = 1;  /* allow re-init for fresh parse */\n";
        out << "    return yyresult;\n";
        out << "}\n";

        // For "both" mode, also emit yyparse() that wraps yypstate/yypush_parse.
        if (push_both) {
            out << "int yyparse(" << params_decl_signature() << ") {\n";
            out << "    yypstate *ps = yypstate_new();\n";
            out << "    if (!ps) return 2;\n";
            out << "    int rc;\n";
            out << "    do {\n";
            out << "        int tok = yylex(" << yylex_call_args() << ");\n";
            out << "        rc = yypush_parse(ps, tok, ";
            out << (pure() ? "&yylval" : "&yylval");
            if (L) out << ", &yylloc";
            for (auto& p : g_.parse_params) {
                size_t end = p.find_last_not_of(" \t\r\n");
                if (end == string::npos) continue;
                size_t start = end;
                while (start > 0 && (ch_isalnum((unsigned char)p[start - 1]) || p[start - 1] == '_'))
                    start--;
                out << ", " << p.substr(start, end - start + 1);
            }
            out << ");\n";
            out << "    } while (rc == YYPUSH_MORE);\n";
            out << "    yypstate_delete(ps);\n";
            out << "    return rc;\n";
            out << "}\n";
        }
    }

    // GLR runtime: tree-of-stacks parser that forks at conflicts, runs
    // all parse paths lock-step over the input, prunes branches that
    // hit errors, and merges branches that converge to the same state.
    // %dprec selects the highest-precedence merge candidate; %merge
    // calls a user-supplied combiner for semantic values.  Locations
    // and parse-params are NOT plumbed through GLR yet — they're a
    // straightforward extension once the deterministic core works.
    //
    // Conflict actions live in two parallel arrays emitted earlier:
    //   yyglr_extra_state[i] / yyglr_extra_token[i] / yyglr_extra_action[i]
    void emit_glr_driver(Buf out) {
        const bool L = g_.want_locations;
        // YYLLOC_DEFAULT (and YYRHSLOC) are defined here too so locations
        // work in GLR action bodies.  Same default as the deterministic
        // driver; users may override by #define before %{ %}.
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
        // Emit the alternate-action table.
        const auto& extras = l_.glr_extra_actions;
        // Sort by (state, token) for binary-search lookup.
        vector<LALR::GlrAction> sorted_extras = extras;
        std::sort(sorted_extras.begin(), sorted_extras.end(),
            [](const LALR::GlrAction& a, const LALR::GlrAction& b) {
                if (a.state != b.state) return a.state < b.state;
                return a.term_internal < b.term_internal;
            });
        out << "static const short yyglr_extra_n = " << sorted_extras.size() << ";\n";
        if (sorted_extras.empty()) {
            out << "static const short yyglr_extra_state[1] = { 0 };\n";
            out << "static const short yyglr_extra_token[1] = { 0 };\n";
            out << "static const short yyglr_extra_action[1] = { 0 };\n";
        } else {
            out << "static const short yyglr_extra_state[" << sorted_extras.size() << "] = {";
            for (size_t i = 0; i < sorted_extras.size(); i++)
                out << (i ? "," : "") << " " << sorted_extras[i].state;
            out << " };\n";
            out << "static const short yyglr_extra_token[" << sorted_extras.size() << "] = {";
            for (size_t i = 0; i < sorted_extras.size(); i++)
                out << (i ? "," : "") << " " << sorted_extras[i].term_internal;
            out << " };\n";
            out << "static const short yyglr_extra_action[" << sorted_extras.size() << "] = {";
            for (size_t i = 0; i < sorted_extras.size(); i++)
                out << (i ? "," : "") << " " << sorted_extras[i].action;
            out << " };\n";
        }

        // Per-rule dprec table.
        out << "static const short yyglr_dprec[" << l_.n_rules() << "] = {";
        for (int p = 0; p < l_.n_rules(); p++)
            out << (p ? "," : "") << " " << l_.prod(p).dprec;
        out << " };\n";

        // %merge function names per rule.  We emit a switch+function
        // call indexed by rule.
        // Bison %merge convention: the user's merger takes (YYSTYPE,
        // YYSTYPE) by value and returns YYSTYPE.  Forward-declare each
        // distinct mergerfn so the user can define it in any translation
        // unit (or after the parser code).
        std::set<std::string> declared;
        for (int p = 0; p < l_.n_rules(); p++) {
            const auto& prod = l_.prod(p);
            if (!prod.merge_fn.empty() && declared.insert(prod.merge_fn).second)
                out << "extern YYSTYPE " << prod.merge_fn
                    << "(YYSTYPE, YYSTYPE);\n";
        }
        out << "/* %merge per-rule resolver. */\n";
        out << "static int yyglr_merge_value(int rule, YYSTYPE *a, YYSTYPE *b, YYSTYPE *out) {\n";
        out << "    switch (rule) {\n";
        for (int p = 0; p < l_.n_rules(); p++) {
            const auto& prod = l_.prod(p);
            if (!prod.merge_fn.empty()) {
                out << "    case " << p << ":\n";
                out << "        *out = " << prod.merge_fn << "(*a, *b); return 1;\n";
            }
        }
        out << "    default: break;\n";
        out << "    }\n";
        out << "    (void)a; (void)b; (void)out; return 0;\n";
        out << "}\n";

        // Tree-of-stacks node + parser.  last_rule is the rule the latest
        // reduce used to produce this node (0 for shifts and the initial
        // node), so the merge resolver can consult yyglr_dprec[] and
        // yyglr_merge_value() when two tops collapse.
        out << "typedef struct yyglr_node {\n";
        out << "    int state;\n";
        out << "    int last_rule;\n";
        out << "    YYSTYPE value;\n";
        if (L) out << "    YYLTYPE loc;\n";
        out << "    struct yyglr_node *prev;\n";
        out << "    int refcount;\n";
        out << "} yyglr_node;\n";

        out << "static yyglr_node *yyglr_node_new(int state, int last_rule, YYSTYPE value, ";
        if (L) out << "YYLTYPE loc, ";
        out << "yyglr_node *prev) {\n";
        out << "    yyglr_node *n = (yyglr_node*)malloc(sizeof(*n));\n";
        out << "    n->state = state; n->last_rule = last_rule;\n";
        out << "    n->value = value;";
        if (L) out << " n->loc = loc;";
        out << " n->prev = prev;\n";
        out << "    n->refcount = 1;\n";
        out << "    if (prev) prev->refcount++;\n";
        out << "    return n;\n";
        out << "}\n";
        out << "static void yyglr_node_release(yyglr_node *n) {\n";
        out << "    while (n && --n->refcount == 0) {\n";
        out << "        yyglr_node *prev = n->prev;\n";
        out << "        free(n);\n";
        out << "        n = prev;\n";
        out << "    }\n";
        out << "}\n";

        // Look up actions for (state, token) — primary plus extras.
        // Returns count and writes up to YYGLR_MAX_ACT actions.
        out << "#define YYGLR_MAX_ACT 4\n";
        out << "static int yyglr_actions_for(int state, int yytoken, int *out_acts, int *out_rules) {\n";
        out << "    int n = 0;\n";
        // Primary via yypact.
        out << "    int yyn = yypact[state];\n";
        out << "    if (!yypact_value_is_default(yyn)) {\n";
        out << "        int idx = yyn + yytoken;\n";
        out << "        if (idx >= 0 && idx < (int)YYTABLE_SIZE && yycheck[idx] == yytoken &&\n";
        out << "            !yytable_value_is_error(yytable[idx])) {\n";
        out << "            int act = yytable[idx];\n";
        out << "            if (n < YYGLR_MAX_ACT) {\n";
        out << "                out_acts[n] = act;\n";
        out << "                out_rules[n] = (act < 0) ? -act : 0;\n";
        out << "                n++;\n";
        out << "            }\n";
        out << "        }\n";
        out << "    }\n";
        // Default reduce when nothing matched.
        out << "    if (n == 0) {\n";
        out << "        int defact = yydefact[state];\n";
        out << "        if (defact != 0 && n < YYGLR_MAX_ACT) {\n";
        out << "            out_acts[n] = -defact;\n";
        out << "            out_rules[n] = defact;\n";
        out << "            n++;\n";
        out << "        }\n";
        out << "    }\n";
        // Extras (binary search).
        out << "    int lo = 0, hi = yyglr_extra_n;\n";
        out << "    while (lo < hi) {\n";
        out << "        int mid = (lo + hi) / 2;\n";
        out << "        int s = yyglr_extra_state[mid];\n";
        out << "        int t = yyglr_extra_token[mid];\n";
        out << "        if (s < state || (s == state && t < yytoken)) lo = mid + 1;\n";
        out << "        else hi = mid;\n";
        out << "    }\n";
        out << "    while (lo < yyglr_extra_n &&\n";
        out << "           yyglr_extra_state[lo] == state &&\n";
        out << "           yyglr_extra_token[lo] == yytoken) {\n";
        out << "        if (n < YYGLR_MAX_ACT) {\n";
        out << "            int act = yyglr_extra_action[lo];\n";
        out << "            out_acts[n] = act;\n";
        out << "            out_rules[n] = (act < 0) ? -act : 0;\n";
        out << "            n++;\n";
        out << "        }\n";
        out << "        lo++;\n";
        out << "    }\n";
        out << "    return n;\n";
        out << "}\n";

        // Action switch.  Same shape as the deterministic switch but
        // uses a local yyvsp for resolved actions during reduce.  When
        // %locations is on, the LHS yyloc is computed by the caller via
        // YYLLOC_DEFAULT and passed in; the action body may further
        // adjust it via @$.  yylsp points to the top RHS location so
        // @N references work.
        out << "static YYSTYPE yyglr_run_action(int rule, YYSTYPE *vals_top";
        if (L) out << ", YYLTYPE *locs_top, YYLTYPE *p_yyloc";
        for (auto& p : g_.parse_params) out << ", " << p;
        out << ") {\n";
        out << "    YYSTYPE yyval;\n";
        out << "    YYSTYPE *yyvsp = vals_top;\n";
        if (L) {
            out << "    YYLTYPE yyloc = *p_yyloc;\n";
            out << "    YYLTYPE *yylsp = locs_top;\n";
            out << "    (void)yylsp;\n";
        }
        out << "    int yylen;\n";
        out << "    switch (rule) {\n";
        for (int i = 1; i < l_.n_rules(); i++) {
            const Production& p = l_.prod(i);
            int len = (int)p.rhs.size();
            out << "    case " << i << ":\n";
            out << "        yylen = " << len << ";\n";
            out << "        if (yylen) yyval = yyvsp[1 - yylen];\n";
            out << "        else memset(&yyval, 0, sizeof(yyval));\n";
            if (!p.action.empty()) {
                out << "        { " << translate_action(p) << " }\n";
            }
            out << "        break;\n";
        }
        out << "    default: memset(&yyval, 0, sizeof(yyval)); break;\n";
        out << "    }\n";
        if (L) out << "    *p_yyloc = yyloc;\n";
        out << "    return yyval;\n";
        out << "}\n";

        // The driver itself.  Tops grow dynamically via realloc — no
        // hard cap on simultaneous parse paths.  YYGLR_MAX_RHS bounds the
        // values[] buffer used during a single reduce; computed below as
        // the largest RHS length across all rules.
        int max_rhs_len = 1;
        for (int p = 0; p < l_.n_rules(); p++)
            max_rhs_len = std::max(max_rhs_len, (int)l_.prod(p).rhs.size());
        out << "#define YYGLR_MAX_RHS " << (max_rhs_len + 1) << "\n";
        out << "extern int yylex(void);\n";
        out << "static void yyglr_grow(yyglr_node ***arr, int *cap, int need) {\n";
        out << "    if (*cap >= need) return;\n";
        out << "    int nc = *cap ? *cap : 16;\n";
        out << "    while (nc < need) nc *= 2;\n";
        out << "    *arr = (yyglr_node**)realloc(*arr, nc * sizeof(**arr));\n";
        out << "    *cap = nc;\n";
        out << "}\n";
        out << "int yyparse(" << params_decl_signature() << ") {\n";
        out << "    /* Active stack tops; each is a yyglr_node*.  Heap-allocated\n";
        out << "       and grown on demand. */\n";
        out << "    int tops_cap = 16, next_cap = 16;\n";
        out << "    yyglr_node **tops = (yyglr_node**)malloc(tops_cap * sizeof(*tops));\n";
        out << "    yyglr_node **next_tops = (yyglr_node**)malloc(next_cap * sizeof(*next_tops));\n";
        out << "    YYSTYPE init_val; memset(&init_val, 0, sizeof(init_val));\n";
        if (L) out << "    YYLTYPE init_loc; memset(&init_loc, 0, sizeof(init_loc));\n";
        out << "    tops[0] = yyglr_node_new(0, 0, init_val, ";
        if (L) out << "init_loc, ";
        out << "NULL);\n";
        out << "    int n_tops = 1;\n";
        out << "    yychar = -2;\n";
        out << "    yynerrs = 0;\n";
        out << "    int result = 1; /* default: error */\n";
        out << "    for (;;) {\n";
        out << "        if (yychar == -2) yychar = yylex();\n";
        out << "        int yytoken = (yychar <= 0) ? 0 : YYTRANSLATE(yychar);\n";
        out << "        int n_next = 0;\n";
        out << "        int any_accept = 0;\n";
        out << "        int progress = 0; /* did anything advance? */\n";
        out << "        int did_shift = 0; /* did any top consume the token? */\n";
        out << "        for (int i = 0; i < n_tops; i++) {\n";
        out << "            yyglr_node *top = tops[i];\n";
        out << "            int acts[YYGLR_MAX_ACT]; int rules[YYGLR_MAX_ACT];\n";
        out << "            int n_acts = yyglr_actions_for(top->state, yytoken, acts, rules);\n";
        out << "            if (n_acts == 0) {\n";
        out << "                yyglr_node_release(top);\n";
        out << "                continue;\n";
        out << "            }\n";
        out << "            for (int k = 0; k < n_acts; k++) {\n";
        out << "                int act = acts[k];\n";
        out << "                if (act == 0) { any_accept = 1; continue; }\n";
        out << "                if (act > 0) {\n";
        out << "                    /* Shift to state act. */\n";
        out << "                    yyglr_grow(&next_tops, &next_cap, n_next + 1);\n";
        out << "                    yyglr_node *n = yyglr_node_new(act, 0, yylval, ";
        if (L) out << "yylloc, ";
        out << "top);\n";
        out << "                    next_tops[n_next++] = n;\n";
        out << "                    progress = 1;\n";
        out << "                    did_shift = 1;\n";
        out << "                } else {\n";
        out << "                    /* Reduce by rule -act. */\n";
        out << "                    int rule = -act;\n";
        out << "                    int len = yyr2[rule];\n";
        out << "                    yyglr_node *cur = top;\n";
        out << "                    YYSTYPE values[YYGLR_MAX_RHS];\n";
        if (L) out << "                    YYLTYPE locs[YYGLR_MAX_RHS + 1];\n";
        out << "                    if (len >= YYGLR_MAX_RHS) { /* too deep */ continue; }\n";
        out << "                    for (int j = len; j > 0; j--) {\n";
        out << "                        if (!cur) break;\n";
        out << "                        values[j-1] = cur->value;\n";
        if (L) out << "                        locs[j] = cur->loc;\n";
        out << "                        cur = cur->prev;\n";
        out << "                    }\n";
        out << "                    if (!cur && len > 0) continue;\n";
        out << "                    int prevstate = cur ? cur->state : 0;\n";
        if (L) {
            out << "                    { YYLTYPE z; memset(&z, 0, sizeof(z));\n";
            out << "                      locs[0] = cur ? cur->loc : z; }\n";
            out << "                    YYLTYPE yyloc; YYLLOC_DEFAULT(yyloc, locs, len);\n";
        }
        out << "                    YYSTYPE *valptr = (len > 0) ? &values[len-1] : &values[0];\n";
        if (L) out << "                    YYLTYPE *locptr = &locs[len];\n";
        out << "                    YYSTYPE yyval = yyglr_run_action(rule, valptr";
        if (L) out << ", locptr, &yyloc";
        out << params_call(g_.parse_params) << ");\n";
        out << "                    /* GOTO */\n";
        out << "                    int yylhs = yyr1[rule];\n";
        out << "                    int nt = yylhs - YYNTOKENS;\n";
        out << "                    int gpos = yypgoto[nt] + prevstate;\n";
        out << "                    int gostate;\n";
        out << "                    if (gpos >= 0 && gpos < (int)YYTABLE_SIZE && yycheck[gpos] == prevstate)\n";
        out << "                        gostate = yytable[gpos];\n";
        out << "                    else\n";
        out << "                        gostate = yydefgoto[nt];\n";
        out << "                    yyglr_grow(&next_tops, &next_cap, n_next + 1);\n";
        out << "                    yyglr_node *n = yyglr_node_new(gostate, rule, yyval, ";
        if (L) out << "yyloc, ";
        out << "cur);\n";
        out << "                    next_tops[n_next++] = n;\n";
        out << "                    progress = 1;\n";
        out << "                    /* Re-process this node against the same token. */\n";
        out << "                    /* (We approximate by appending back; it'll be picked\n";
        out << "                     * up in the next iteration of the outer for loop.) */\n";
        out << "                }\n";
        out << "            }\n";
        out << "            yyglr_node_release(top);\n";
        out << "        }\n";
        // Merge tops with the same (state, prev).  Two parses converged.
        // Resolution order, matching Bison:
        //   1. If both reductions name a %merge function, call it on the
        //      two semantic values and keep the merged node.
        //   2. Otherwise, if either side has a higher %dprec, that side
        //      wins and the loser is dropped.
        //   3. Otherwise (no dprec, no merge), drop the second arbitrarily.
        out << "        for (int i = 0; i < n_next; i++) {\n";
        out << "            for (int j = i+1; j < n_next; j++) {\n";
        out << "                if (!next_tops[i] || !next_tops[j]) continue;\n";
        out << "                if (next_tops[i]->state != next_tops[j]->state ||\n";
        out << "                    next_tops[i]->prev  != next_tops[j]->prev) continue;\n";
        out << "                int ri = next_tops[i]->last_rule;\n";
        out << "                int rj = next_tops[j]->last_rule;\n";
        out << "                YYSTYPE merged;\n";
        out << "                if (ri > 0 && rj > 0 &&\n";
        out << "                    yyglr_merge_value(ri, &next_tops[i]->value,\n";
        out << "                                          &next_tops[j]->value, &merged)) {\n";
        out << "                    next_tops[i]->value = merged;\n";
        out << "                    yyglr_node_release(next_tops[j]);\n";
        out << "                    next_tops[j] = NULL;\n";
        out << "                    continue;\n";
        out << "                }\n";
        out << "                int di = (ri > 0) ? yyglr_dprec[ri] : 0;\n";
        out << "                int dj = (rj > 0) ? yyglr_dprec[rj] : 0;\n";
        out << "                if (di < dj) {\n";
        out << "                    yyglr_node_release(next_tops[i]);\n";
        out << "                    next_tops[i] = next_tops[j];\n";
        out << "                    next_tops[j] = NULL;\n";
        out << "                } else {\n";
        out << "                    yyglr_node_release(next_tops[j]);\n";
        out << "                    next_tops[j] = NULL;\n";
        out << "                }\n";
        out << "            }\n";
        out << "        }\n";
        out << "        int compact = 0;\n";
        out << "        for (int i = 0; i < n_next; i++) {\n";
        out << "            if (next_tops[i]) next_tops[compact++] = next_tops[i];\n";
        out << "        }\n";
        out << "        n_next = compact;\n";
        // Swap.
        out << "        yyglr_node **tmp = tops; tops = next_tops; next_tops = tmp;\n";
        out << "        int tcap_tmp = tops_cap; tops_cap = next_cap; next_cap = tcap_tmp;\n";
        out << "        n_tops = n_next;\n";
        out << "        if (any_accept) { result = 0; break; }\n";
        out << "        if (n_tops == 0) {\n";
        out << "            yyerror(\"syntax error\");\n";
        out << "            yynerrs++;\n";
        out << "            result = 1;\n";
        out << "            break;\n";
        out << "        }\n";
        // Consume the token only when at least one top shifted it this
        // iteration.  Reductions never consume — they need to keep folding
        // the stacks until a shift or accept happens.
        out << "        if (did_shift) yychar = -2;\n";
        // Accept condition: any top in the YYFINAL state.
        out << "        for (int i = 0; i < n_tops; i++) {\n";
        out << "            if (tops[i]->state == YYFINAL) { result = 0; goto yyglr_done; }\n";
        out << "        }\n";
        // No progress on a non-EOF token: error.  On EOF specifically, no
        // progress + no accept = error.
        out << "        if (progress == 0) {\n";
        out << "            if (yychar != 0) { yyerror(\"syntax error\"); yynerrs++; }\n";
        out << "            else { yyerror(\"syntax error at end of input\"); yynerrs++; }\n";
        out << "            break;\n";
        out << "        }\n";
        out << "    }\n";
        out << "yyglr_done:\n";
        out << "    for (int i = 0; i < n_tops; i++) yyglr_node_release(tops[i]);\n";
        out << "    free(tops);\n";
        out << "    free(next_tops);\n";
        out << "    return result;\n";
        out << "}\n";
    }
};

// ============================================================================
// Verbose report (-v / --report=*) — Bison-compatible .output file.
// ============================================================================

static string write_report(const Grammar& g, const LALR& l) {
    std::string s;
    Buf out{s};
    auto sym_name = [&](int internal) -> string {
        int sym = l.internal_to_sym(internal);
        if (sym == g.eof_sym) return "$end";
        return g.syms[sym].display.empty() ? g.syms[sym].name : g.syms[sym].display;
    };

    out << "Terminals, with rules where they appear\n\n";
    for (int i = 0; i < l.n_terminals(); i++) {
        out << "    " << sym_name(i) << " ";
        int sym = l.internal_to_sym(i);
        if (g.syms[sym].code >= 0) out << "(" << g.syms[sym].code << ")";
        out << "\n";
    }
    out << "\n\nNonterminals, with rules where they appear\n\n";
    for (int i = l.n_terminals(); i < l.n_total_syms(); i++) {
        out << "    " << sym_name(i) << "\n";
    }
    out << "\n\nGrammar\n\n";
    for (int p = 0; p < l.n_rules(); p++) {
        const auto& prod = l.prod(p);
        out << "    " << p << " " << g.syms[prod.lhs].name << ":";
        if (prod.rhs.empty()) out << " %empty";
        for (int s : prod.rhs) out << " " << g.syms[s].display;
        out << "\n";
    }
    out << "\n\n";

    // Per-state listing.
    for (int s = 0; s < l.n_states(); s++) {
        out << "state " << s << "\n\n";
        const State& st = l.state(s);
        for (auto& it : st.items) {
            const auto& prod = l.prod(it.prod);
            out << "    " << it.prod << " " << g.syms[prod.lhs].name << ":";
            for (size_t k = 0; k < prod.rhs.size(); k++) {
                if (k == it.dot) out << " .";
                out << " " << g.syms[prod.rhs[k]].display;
            }
            if (it.dot == prod.rhs.size()) out << " .";
            out << "\n";
        }
        out << "\n";
        // Actions per terminal.
        for (int t = 0; t < l.n_terminals(); t++) {
            int a = l.action(s, t);
            if (a == 0) continue;
            out << "    " << sym_name(t) << "  ";
            if (a == LALR::ACCEPT) out << "accept\n";
            else if (a == LALR::ERR_MARK) out << "error (nonassociative)\n";
            else if (a > 0) out << "shift, and go to state " << (a - 1) << "\n";
            else out << "reduce using rule " << (-a) << "\n";
        }
        // Gotos per nonterminal.
        for (int nt = 0; nt < l.n_nonterminals(); nt++) {
            int g_dst = l.goto_tab(s, nt);
            if (g_dst == 0) continue;
            int sym = l.internal_to_sym(l.n_terminals() + nt);
            out << "    " << g.syms[sym].name << "  go to state " << (g_dst - 1) << "\n";
        }
        out << "\n";
    }
    return s;
}

// Graphviz .dot rendering of the LR automaton.  One node per state with the
// kernel items as the label; edges are labelled by the symbol that triggers
// the transition.
static string write_graph(const Grammar& g, const LALR& l) {
    auto esc = [](const string& s) {
        string r;
        for (char c : s) {
            if (c == '"' || c == '\\') { r += '\\'; r += c; }
            else if (c == '\n') r += "\\n";
            else r += c;
        }
        return r;
    };
    auto sym_display = [&](int internal) -> string {
        int sym = l.internal_to_sym(internal);
        if (sym == g.eof_sym) return "$end";
        return g.syms[sym].display.empty() ? g.syms[sym].name : g.syms[sym].display;
    };
    string s;
    Buf out{s};
    out << "// LR automaton, generated by yacc.cpp\n";
    out << "digraph yacc {\n";
    out << "  rankdir=LR;\n";
    out << "  node [shape=box, fontname=\"monospace\", fontsize=10];\n";
    for (int sid = 0; sid < l.n_states(); sid++) {
        const State& st = l.state(sid);
        string label = "State " + std::to_string(sid);
        for (auto& it : st.kernel) {
            const auto& prod = l.prod(it.prod);
            label += "\\n" + g.syms[prod.lhs].name + " ->";
            for (size_t k = 0; k < prod.rhs.size(); k++) {
                if (k == it.dot) label += " .";
                label += " " + g.syms[prod.rhs[k]].display;
            }
            if (it.dot == prod.rhs.size()) label += " .";
        }
        out << "  s" << sid << " [label=\"" << esc(label) << "\"];\n";
    }
    for (int sid = 0; sid < l.n_states(); sid++) {
        const State& st = l.state(sid);
        for (auto& [X, dst] : st.trans) {
            int internal = l.sym_to_internal(X);
            out << "  s" << sid << " -> s" << dst
                << " [label=\"" << esc(sym_display(internal)) << "\"];\n";
        }
    }
    out << "}\n";
    return s;
}

// Bison-compatible XML report.  Per-state listing of items, actions, and
// transitions, similar to what `bison -x` produces.  We emit a small,
// well-formed subset (structure first; we don't try to byte-match Bison).
static string write_xml(const Grammar& g, const LALR& l) {
    auto esc = [](const string& s) {
        string r;
        for (char c : s) {
            if (c == '<') r += "&lt;";
            else if (c == '>') r += "&gt;";
            else if (c == '&') r += "&amp;";
            else if (c == '"') r += "&quot;";
            else r += c;
        }
        return r;
    };
    auto sym_display = [&](int internal) -> string {
        int sym = l.internal_to_sym(internal);
        if (sym == g.eof_sym) return "$end";
        return g.syms[sym].display.empty() ? g.syms[sym].name : g.syms[sym].display;
    };
    string s;
    Buf out{s};
    out << "<?xml version=\"1.0\"?>\n";
    out << "<bison-xml-report version=\"1.0\">\n";
    out << "  <grammar>\n";
    out << "    <terminals>\n";
    for (int i = 0; i < l.n_terminals(); i++) {
        int sym = l.internal_to_sym(i);
        out << "      <terminal symbol-number=\"" << i << "\" name=\""
            << esc(sym_display(i)) << "\" token-number=\""
            << g.syms[sym].code << "\"/>\n";
    }
    out << "    </terminals>\n";
    out << "    <nonterminals>\n";
    for (int i = l.n_terminals(); i < l.n_total_syms(); i++) {
        out << "      <nonterminal symbol-number=\"" << i << "\" name=\""
            << esc(sym_display(i)) << "\"/>\n";
    }
    out << "    </nonterminals>\n";
    out << "    <rules>\n";
    for (int p = 0; p < l.n_rules(); p++) {
        const auto& prod = l.prod(p);
        out << "      <rule number=\"" << p << "\">\n";
        out << "        <lhs>" << esc(g.syms[prod.lhs].name) << "</lhs>\n";
        out << "        <rhs>";
        for (int s : prod.rhs) out << "<symbol>" << esc(g.syms[s].display) << "</symbol>";
        if (prod.rhs.empty()) out << "<empty/>";
        out << "</rhs>\n";
        out << "      </rule>\n";
    }
    out << "    </rules>\n";
    out << "  </grammar>\n";
    out << "  <automaton>\n";
    for (int sid = 0; sid < l.n_states(); sid++) {
        out << "    <state number=\"" << sid << "\">\n";
        const State& st = l.state(sid);
        out << "      <itemset>\n";
        for (auto& it : st.kernel) {
            const auto& prod = l.prod(it.prod);
            out << "        <item rule-number=\"" << it.prod
                << "\" point=\"" << it.dot << "\"/>\n";
        }
        out << "      </itemset>\n";
        out << "      <transitions>\n";
        for (auto& [X, dst] : st.trans) {
            int internal = l.sym_to_internal(X);
            out << "        <transition type=\""
                << (g.syms[X].is_terminal ? "shift" : "goto")
                << "\" symbol=\"" << esc(sym_display(internal))
                << "\" state=\"" << dst << "\"/>\n";
        }
        out << "      </transitions>\n";
        out << "    </state>\n";
    }
    out << "  </automaton>\n";
    out << "</bison-xml-report>\n";
    return s;
}

// Basic counter-example: for each conflict (state, token), do a BFS from
// state 0 over the transitions to find a shortest path to the conflict
// state, then print the path's symbol sequence followed by the lookahead
// at the dot.  This is a simpler diagnostic than Bison's full
// Isradisaikul/Myers (PLDI 2015) algorithm, but enough to point the user
// at what input triggers the conflict.
static string write_counterexamples(const Grammar& g, const LALR& l) {
    if (l.conflicts.empty()) return string();
    // BFS: predecessor map per state.  states_ has trans (sym -> dst).  We
    // walk forward from 0 and record predecessor + entering-symbol.
    int n = l.n_states();
    vector<int> prev(n, -1);
    vector<int> via(n, -1);
    vector<bool> seen(n, false);
    std::vector<int> q = {0};
    seen[0] = true;
    for (size_t i = 0; i < q.size(); i++) {
        int s = q[i];
        for (auto& [X, dst] : l.state(s).trans) {
            if (seen[dst]) continue;
            seen[dst] = true;
            prev[dst] = s;
            via[dst] = X;
            q.push_back(dst);
        }
    }
    auto path_to = [&](int s) -> string {
        vector<int> syms;
        for (int cur = s; cur > 0; cur = prev[cur]) {
            if (via[cur] >= 0) syms.push_back(via[cur]);
        }
        std::reverse(syms.begin(), syms.end());
        string out;
        for (int sym : syms) {
            if (!out.empty()) out += ' ';
            out += g.syms[sym].display.empty() ? g.syms[sym].name : g.syms[sym].display;
        }
        return out;
    };
    auto sym_name = [&](int sym) -> string {
        return g.syms[sym].display.empty() ? g.syms[sym].name : g.syms[sym].display;
    };
    auto fmt_item = [&](const Item& item, int dot_override = -1) -> string {
        const auto& prod = l.prod(item.prod);
        int dot = (dot_override >= 0) ? dot_override : item.dot;
        string r = g.syms[prod.lhs].name + " ->";
        if (prod.rhs.empty()) r += " %empty";
        for (int j = 0; j < (int)prod.rhs.size(); j++) {
            if (j == dot) r += " .";
            r += " " + sym_name(prod.rhs[j]);
        }
        if (dot == (int)prod.rhs.size()) r += " .";
        return r;
    };
    string s;
    Buf out{s};
    for (const auto& c : l.conflicts) {
        const State& st = l.state(c.state);
        const char* kind = (c.kind == 1) ? "shift/reduce" : "reduce/reduce";
        int tsym = l.internal_to_sym(c.term_internal);
        const string& tname = sym_name(tsym);

        out << "yacc: " << kind << " conflict in state " << c.state
            << " on " << tname << "\n";
        out << "  Example: " << path_to(c.state) << " . " << tname << "\n";

        // Walk the state's full closure and show items that take action on
        // this terminal: a shift item has the conflicting terminal right
        // after the dot; a reduce item has dot at end with the terminal in
        // its lookahead (kernel only — closure items inherit lookaheads
        // from kernel, and listing the kernel is enough to identify the
        // reductions involved).
        bool printed_shift = false;
        for (const auto& item : st.items) {
            const auto& prod = l.prod(item.prod);
            if ((int)item.dot >= (int)prod.rhs.size()) continue;
            if (prod.rhs[item.dot] == tsym) {
                if (!printed_shift) {
                    out << "  Shift derivation:\n";
                    printed_shift = true;
                }
                out << "    " << fmt_item(item) << "\n";
            }
        }
        bool printed_reduce = false;
        for (size_t i = 0; i < st.kernel.size(); i++) {
            const auto& item = st.kernel[i];
            const auto& prod = l.prod(item.prod);
            if ((int)item.dot != (int)prod.rhs.size()) continue;
            bool covers = (i < st.la.size())
                       && (st.la[i].count(c.term_internal) > 0);
            if (!covers) continue;
            if (!printed_reduce) {
                out << "  Reduce derivation:\n";
                printed_reduce = true;
            }
            out << "    " << fmt_item(item) << "\n";
        }
    }
    return s;
}

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
        else if (a == "-g" || a == "--graph") opts.want_graph = true;
        else if (starts("--graph=")) { opts.want_graph = true; opts.graph_path = a.substr(8); }
        else if (a == "-x" || a == "--xml")   opts.want_xml = true;
        else if (starts("--xml="))   { opts.want_xml = true; opts.xml_path = a.substr(6); }
        else if (a == "-V" || a == "--version") {
            write_stdout("yacc.cpp 0.1.0 (bison-compatible parser generator)\n");
            return 0;
        }
        else if (a == "-h" || a == "--help") {
            write_stdout("Usage: yacc [OPTION]... FILE\n");
            return 0;
        }
        else if (a == "-Wcounterexamples" || a == "-Wcex" ||
                 a == "--counterexamples") {
            opts.want_counterexamples = true;
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
    if (opts.debug) g.parse_trace = true;
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

    // Source-stem helper for -v/-g/-x output naming.
    auto stem_path = [&](const string& ext) {
        size_t slash = input.find_last_of("/\\");
        string fname = (slash == string::npos) ? input : input.substr(slash + 1);
        size_t dot = fname.find_last_of('.');
        string stem = (dot == string::npos) ? fname : fname.substr(0, dot);
        return stem + ext;
    };

    // -v / --verbose: write a Bison-compatible .output report file with
    // state listings, items, actions, and gotos.
    if (opts.verbose) {
        string rpath = stem_path(".output");
        string report = write_report(g, la);
        if (!write_file(rpath, report))
            fatalf("cannot write report '{}'", rpath);
    }
    // -g / --graph: Graphviz .dot of the LR automaton.
    if (opts.want_graph) {
        string gpath = opts.graph_path.empty() ? stem_path(".dot") : opts.graph_path;
        if (!write_file(gpath, write_graph(g, la)))
            fatalf("cannot write graph '{}'", gpath);
    }
    // -x / --xml: Bison-compatible XML report.
    if (opts.want_xml) {
        string xpath = opts.xml_path.empty() ? stem_path(".xml") : opts.xml_path;
        if (!write_file(xpath, write_xml(g, la)))
            fatalf("cannot write xml '{}'", xpath);
    }

    // Suppress the conflict warning when %expect / %expect-rr matches the
    // actual count.  Bison does the same, and projects (Octave, PG, Bash)
    // declare exact %expect to gate a clean build.
    int sr_unexpected = la.sr_conflicts();
    int rr_unexpected = la.rr_conflicts();
    if (g.expected_sr >= 0 && g.expected_sr == sr_unexpected) sr_unexpected = 0;
    if (g.expected_rr >= 0 && g.expected_rr == rr_unexpected) rr_unexpected = 0;
    if (sr_unexpected > 0)
        write_stderr(std::format("yacc: {} shift/reduce conflict(s)\n", la.sr_conflicts()));
    if (rr_unexpected > 0)
        write_stderr(std::format("yacc: {} reduce/reduce conflict(s)\n", la.rr_conflicts()));
    // -Wcounterexamples: list each conflict with a sample input path.
    if (opts.want_counterexamples && !la.conflicts.empty()) {
        write_stderr(write_counterexamples(g, la));
    }
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
