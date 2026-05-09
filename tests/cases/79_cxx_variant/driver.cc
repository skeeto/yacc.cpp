#include "grammar.tab.hh"
#include <cstdio>
#include <string>

static int idx;
int yylex(yy::parser::semantic_type *yylval) {
    static const int t[] = {
        yy::parser::token::NUM, '+', yy::parser::token::NUM, 0
    };
    static const int v[] = { 12, 0, 30 };
    if (t[idx] == 0) return 0;
    if (t[idx] == yy::parser::token::NUM) yylval->emplace<int>(v[idx]);
    return t[idx++];
}

namespace yy {
    void parser::error(const std::string& msg) {
        std::fprintf(stderr, "%s\n", msg.c_str());
    }
}

int main() {
    yy::parser p;
    return p.parse();
}
