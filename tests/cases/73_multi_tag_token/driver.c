#include <stdio.h>
#include "grammar.tab.h"
static int idx;
int yylex(void) {
    static const int t[] = { X, 0 };
    if (t[idx] == 0) return 0;
    yylval.ival = 42;
    return t[idx++];
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
