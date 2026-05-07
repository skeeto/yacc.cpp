#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grammar.tab.h"
extern YYSTYPE yylval;

static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = (char*)malloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

int yylex(void) {
    int c;
    do { c = getchar(); } while (c == ' ' || c == '\t');
    if (c == EOF) return 0;
    if (c >= '0' && c <= '9') {
        int v = 0;
        do { v = v*10 + (c - '0'); c = getchar(); } while (c >= '0' && c <= '9');
        if (c != EOF) ungetc(c, stdin);
        yylval.ival = v; return INT;
    }
    if (c == '"') {
        char buf[256]; int n = 0;
        while ((c = getchar()) != EOF && c != '"' && n < 255) buf[n++] = c;
        buf[n] = 0;
        yylval.sval = xstrdup(buf); return STR;
    }
    return c;
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
