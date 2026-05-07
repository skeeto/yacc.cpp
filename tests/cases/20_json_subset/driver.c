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
    do { c = getchar(); } while (c == ' ' || c == '\t' || c == '\n' || c == '\r');
    if (c == EOF) return 0;
    if (c == '-' || (c >= '0' && c <= '9')) {
        long sign = 1;
        if (c == '-') { sign = -1; c = getchar(); }
        long v = 0;
        while (c >= '0' && c <= '9') { v = v*10 + (c - '0'); c = getchar(); }
        if (c != EOF) ungetc(c, stdin);
        yylval.ival = sign * v;
        return NUMBER;
    }
    if (c == '"') {
        char buf[1024]; int n = 0;
        while ((c = getchar()) != EOF && c != '"' && n < 1023) buf[n++] = c;
        buf[n] = 0;
        yylval.sval = xstrdup(buf);
        return STRING;
    }
    if (c == 't') {
        getchar(); getchar(); getchar(); return TRUE;
    }
    if (c == 'f') {
        getchar(); getchar(); getchar(); getchar(); return FALSE;
    }
    if (c == 'n') {
        getchar(); getchar(); getchar(); return NULL_TOK;
    }
    return c;
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
