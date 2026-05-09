#include "grammar.tab.hh"
#include <cstdio>

static int idx;
int yylex(yy::parser::semantic_type*, yy::parser::location_type* loc) {
    static const int t[] = { yy::parser::token::NUM, 0 };
    if (t[idx] == 0) return 0;
    loc->begin.line = 4; loc->begin.column = 7;
    loc->end.line   = 4; loc->end.column   = 11;
    return t[idx++];
}

namespace yy {
    void parser::error(const location_type&, const std::string& msg) {
        std::fprintf(stderr, "%s\n", msg.c_str());
    }
}

int main() { yy::parser p; return p.parse(); }
