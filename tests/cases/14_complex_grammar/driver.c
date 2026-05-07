#include <stdio.h>
#include <ctype.h>
#include "grammar.tab.h"
extern YYSTYPE yylval;

static int peeked = -1;
static int next_char(void) {
    if (peeked != -1) { int c = peeked; peeked = -1; return c; }
    return getchar();
}
static void unput_char(int c) { peeked = c; }

int yylex(void) {
    int c;
    do { c = next_char(); } while (c == ' ' || c == '\t' || c == '\n');
    if (c == EOF) return 0;
    if (c >= '0' && c <= '9') {
        int v = 0;
        do { v = v*10 + (c - '0'); c = next_char(); } while (c >= '0' && c <= '9');
        if (c != EOF) unput_char(c);
        yylval = v; return NUMBER;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        char buf[64]; int n = 0;
        do { buf[n++] = c; c = next_char(); } while (n < 60 && (
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_'));
        buf[n] = 0;
        if (c != EOF) unput_char(c);
        return IDENT;
    }
    if (c == '<') {
        int d = next_char();
        if (d == '=') return LE;
        if (d != EOF) unput_char(d);
        return c;
    }
    if (c == '>') {
        int d = next_char();
        if (d == '=') return GE;
        if (d != EOF) unput_char(d);
        return c;
    }
    if (c == '=') {
        int d = next_char();
        if (d == '=') return EQ;
        if (d != EOF) unput_char(d);
        return c;
    }
    if (c == '!') {
        int d = next_char();
        if (d == '=') return NE;
        if (d != EOF) unput_char(d);
        return NOT;
    }
    return c;
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
