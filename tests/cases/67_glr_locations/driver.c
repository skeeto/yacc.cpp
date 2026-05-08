#include <stdio.h>
#include "grammar.tab.h"

extern YYLTYPE yylloc;
static int idx;

int yylex(void) {
    static const int t[]      = { A, B, 0 };
    static const int line[]   = { 1, 2 };
    static const int col_s[]  = { 1, 5 };
    static const int col_e[]  = { 3, 7 };
    if (t[idx] == 0) return 0;
    yylloc.first_line   = line[idx];
    yylloc.first_column = col_s[idx];
    yylloc.last_line    = line[idx];
    yylloc.last_column  = col_e[idx];
    return t[idx++];
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
