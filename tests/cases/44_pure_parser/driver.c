#include <stdio.h>
#include "grammar.tab.h"

int yylex(YYSTYPE *lvalp) {
    int c;
    do { c = getchar(); } while (c == ' ' || c == '\t');
    if (c == EOF) return 0;
    if (c >= '0' && c <= '9') {
        int v = 0;
        do { v = v*10 + (c - '0'); c = getchar(); } while (c >= '0' && c <= '9');
        if (c != EOF) ungetc(c, stdin);
        *lvalp = v;
        return NUMBER;
    }
    return c;
}

void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
