#include "grammar.tab.hh"
#include <cstdio>

namespace yy {
    void parser::error(const std::string& msg) {
        std::fprintf(stderr, "%s\n", msg.c_str());
    }
}

int main() {
    yy::parser p;
    using T = yy::parser::token;
    // Feed tokens one at a time.  Each push_parse returns YYPUSH_MORE
    // until enough input has been seen to accept (or to fail).
    struct Tok { int kind; int val; };
    Tok seq[] = {
        {T::NUM,  3},
        {T::PLUS, 0},
        {T::NUM,  4},
        {0,       0},
    };
    int rc = yy::parser::YYPUSH_MORE;
    for (auto& t : seq) {
        rc = p.push_parse(t.kind, t.val);
        if (rc != yy::parser::YYPUSH_MORE) break;
    }
    return rc;
}
