#include <stdio.h>
#include "grammar.tab.h"
static int idx;
int yylex(void) {
    static const int t[] = { PLUS, EPILOGUE, 0 };
    static const int v[] = { 3, 4 };
    if (t[idx] == 0) return 0;
    yylval.n = v[idx];
    return t[idx++];
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
