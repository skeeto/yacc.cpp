#include <stdio.h>
#include "grammar.tab.h"
static int idx;
static int toks_init;
static int toks[8];
int yylex(void) { return toks[idx++]; }
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) {
    toks[0] = A; toks[1] = B; toks[2] = C; toks[3] = 0;
    return yyparse();
}
