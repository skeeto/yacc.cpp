#include <stdio.h>
#include "grammar.tab.h"

static int idx;
int yylex(void) {
    static const int t[] = { A, 0 };
    return t[idx++];
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) {
    int out = 0;
    return yyparse(&out);
}
