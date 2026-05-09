#include "grammar.tab.hh"
#include <cstdio>

static int n = 0;
int yylex(yy::parser::semantic_type *) {
    if (n++ < 1024) return yy::parser::token::A;
    return 0;
}

namespace yy {
    void parser::error(const std::string& msg) {
        std::fprintf(stderr, "%s\n", msg.c_str());
    }
}

int main() {
    yy::parser p;
    int rc = p.parse();
    if (rc == 0) std::printf("ok\n");
    return rc;
}
