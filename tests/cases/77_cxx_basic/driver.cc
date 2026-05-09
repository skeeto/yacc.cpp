#include "grammar.tab.hh"
#include <cstdio>

static int idx;
int yylex(yy::parser::semantic_type *yylval) {
    static const int t[] = {
        yy::parser::token::NUM,
        yy::parser::token::NUM,
        yy::parser::token::NUM,
        0
    };
    static const int v[] = { 7, 8, 9 };
    if (t[idx] == 0) return 0;
    *yylval = v[idx];
    return t[idx++];
}

// Bison's lalr1.cc declares error() but doesn't define it; users must
// override.  Our skeleton provides a default, but to stay compatible
// with both generators the test supplies its own.
namespace yy {
    void parser::error(const std::string& msg) {
        std::fprintf(stderr, "%s\n", msg.c_str());
    }
}

int main() {
    yy::parser p;
    return p.parse();
}
