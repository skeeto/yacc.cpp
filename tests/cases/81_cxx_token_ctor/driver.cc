#include "grammar.tab.hh"
#include <cstdio>

static int idx;
yy::parser::symbol_type yylex() {
    static const int t[] = { 1, 2, 0 };
    if (t[idx] == 0) return yy::parser::symbol_type();  // YYEMPTY but parser treats as EOF
    int kind = t[idx++];
    if (kind == 1) return yy::parser::make_NUM(57);
    return yy::parser::make_EOL();
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
