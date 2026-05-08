#include <stdio.h>
#include "grammar.tab.h"
extern YYLTYPE yylloc;
static int idx;
static struct { int t; int l, c; } toks[] = {
    {A, 1, 1}, {B, 2, 5}, {C, 3, 9}, {0, 0, 0}
};
int yylex(void) {
    if (toks[idx].t == 0) return 0;
    yylloc.first_line = yylloc.last_line = toks[idx].l;
    yylloc.first_column = toks[idx].c;
    yylloc.last_column = toks[idx].c + 1;
    return toks[idx++].t;
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
