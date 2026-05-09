#include "grammar.tab.hh"
#include <cstdio>

static int idx;
int yylex(yy::parser::semantic_type*) {
    // Feed A then EOF: parser expects B, sees EOF -> syntax error
    // with the offending token = S_YYEOF (internal index 0).
    static const int t[] = { yy::parser::token::A, 0 };
    if (t[idx] == 0) return 0;
    return t[idx++];
}

namespace yy {
    void parser::error(const std::string& msg) {
        std::fprintf(stderr, "fallback: %s\n", msg.c_str());
    }
    void parser::report_syntax_error(const context& ctx) const {
        std::fprintf(stderr, "custom: token=%d\n", (int)ctx.token());
    }
}

int main() { yy::parser p; return p.parse(); }
