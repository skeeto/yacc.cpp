#include "grammar.tab.hh"
#include <cstdio>

static int idx;
int yylex(yy::parser::semantic_type *yylval,
          yy::parser::location_type *yylloc) {
    static const int t[] = { yy::parser::token::NUM, 0 };
    static const int v[] = { 99 };
    if (t[idx] == 0) return 0;
    *yylval = v[idx];
    yylloc->begin.line = 3;
    yylloc->begin.column = 5;
    yylloc->end.line = 3;
    yylloc->end.column = 7;
    return t[idx++];
}

namespace yy {
    void parser::error(const location_type& loc, const std::string& msg) {
        std::fprintf(stderr, "%d.%d: %s\n",
                     loc.begin.line, loc.begin.column, msg.c_str());
    }
}

int main() {
    yy::parser p;
    return p.parse();
}
