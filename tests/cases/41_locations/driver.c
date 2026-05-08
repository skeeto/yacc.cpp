#include <stdio.h>
#include "grammar.tab.h"

extern YYSTYPE yylval;
extern YYLTYPE yylloc;

static int cur_line = 1, cur_col = 1;

static int read_ch(void) {
    int c = getchar();
    return c;
}

int yylex(void) {
    int c;
    /* Skip whitespace (not newlines/tabs) but advance the column tracker. */
    for (;;) {
        c = read_ch();
        if (c == ' ' || c == '\t') { cur_col++; continue; }
        break;
    }
    if (c == EOF) return 0;
    yylloc.first_line   = cur_line;
    yylloc.first_column = cur_col;
    if (c >= '0' && c <= '9') {
        int v = c - '0';
        cur_col++;
        for (;;) {
            int d = read_ch();
            if (d < '0' || d > '9') { if (d != EOF) ungetc(d, stdin); break; }
            v = v*10 + (d - '0');
            cur_col++;
        }
        yylloc.last_line   = cur_line;
        yylloc.last_column = cur_col;
        yylval = v;
        return NUMBER;
    }
    if (c == '\n') {
        cur_col++;
        yylloc.last_line = cur_line;
        yylloc.last_column = cur_col;
        cur_line++; cur_col = 1;
        return '\n';
    }
    cur_col++;
    yylloc.last_line   = cur_line;
    yylloc.last_column = cur_col;
    return c;
}

void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
