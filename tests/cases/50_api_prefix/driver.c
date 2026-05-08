#include <stdio.h>
#include "grammar.tab.h"

int foo_lex(void) {
    static int d; if (d) return 0; d = 1;
    foo_lval = 42;
    return NUMBER;
}
void foo_error(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return foo_parse(); }
