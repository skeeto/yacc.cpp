#include "grammar.tab.hh"
#include <cstdio>

static int idx;
int yylex(yy::parser::semantic_type *yylval) {
    static const int t[] = { yy::parser::token::NUM, 0 };
    if (t[idx] == 0) return 0;
    *yylval = 7;
    return t[idx++];
}

namespace yy {
    void parser::error(const std::string& msg) {
        std::fprintf(stderr, "%s\n", msg.c_str());
    }
}

int main() {
    int captured = 0;
    yy::parser p(&captured);
    int rc = p.parse();
    std::printf("captured=%d\n", captured);
    return rc;
}
