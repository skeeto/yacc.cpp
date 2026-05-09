#include "grammar.tab.hh"
#include <cstdio>
#include <memory>

static int idx;
int yylex(yy::parser::semantic_type *yylval) {
    static const int t[] = { yy::parser::token::PTR, 0 };
    if (t[idx] == 0) return 0;
    yylval->emplace<std::unique_ptr<int>>(std::make_unique<int>(123));
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
