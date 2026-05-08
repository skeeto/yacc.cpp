#include <stdio.h>
#include "grammar.tab.h"
extern YYSTYPE yylval;

int yylex(void) {
    int c;
    do { c = getchar(); } while (c == ' ' || c == '\t');
    if (c == EOF) return 0;
    if (c >= '0' && c <= '9') {
        int v = 0;
        do { v = v*10 + (c - '0'); c = getchar(); } while (c >= '0' && c <= '9');
        if (c != EOF) ungetc(c, stdin);
        yylval = v;
        return NUMBER;
    }
    return c;
}

void yyerror(Ctx *ctx, const char *s) { (void)ctx; fprintf(stderr, "%s\n", s); }
int main(void) {
    Ctx ctx = { 0, "running total: " };
    return yyparse(&ctx);
}
