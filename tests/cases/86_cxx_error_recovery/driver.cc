#include "grammar.tab.hh"
#include <cstdio>

static int idx;
int yylex(yy::parser::semantic_type*) {
    // Two statements: a good one, then a malformed one with a missing
    // B that triggers error recovery to the SEMI.
    using T = yy::parser::token;
    static const int t[] = {
        T::A, T::B, T::SEMI,            // valid stmt -> "stmt"
        T::A, T::SEMI,                  // malformed -> "recovered"
        0
    };
    if (t[idx] == 0) return 0;
    return t[idx++];
}

namespace yy {
    void parser::error(const std::string& msg) {
        std::fprintf(stderr, "err: %s\n", msg.c_str());
    }
}

int main() { yy::parser p; return p.parse(); }
