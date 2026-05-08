#include <stdio.h>
#include "grammar.tab.h"

extern YYSTYPE yylval;
extern YYLTYPE yylloc;

static int cur_line = 1, cur_col = 1;

int yylex(void) {
    int c;
    for (;;) {
        c = getchar();
        if (c == ' ' || c == '\t') { cur_col++; continue; }
        break;
    }
    if (c == EOF) return 0;
    yylloc.first_line   = cur_line;
    yylloc.first_column = cur_col;
    yylloc.last_line    = cur_line;
    yylloc.extra        = 99;
    if (c >= '0' && c <= '9') {
        int v = c - '0'; cur_col++;
        for (;;) {
            int d = getchar();
            if (d < '0' || d > '9') { if (d != EOF) ungetc(d, stdin); break; }
            v = v * 10 + (d - '0'); cur_col++;
        }
        yylloc.last_column = cur_col;
        yylval = v;
        return NUMBER;
    }
    cur_col++;
    yylloc.last_column = cur_col;
    if (c == '\n') { cur_line++; cur_col = 1; }
    return c;
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
