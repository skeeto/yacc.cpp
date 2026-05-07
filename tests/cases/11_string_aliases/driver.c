#include <stdio.h>
#include "grammar.tab.h"
extern YYSTYPE yylval;
static int idx;
int yylex(void) {
    static const int toks[] = { IF, NUMBER, THEN, NUMBER, 0 };
    static const int vals[] = { 0,  10,     0,    20,     0 };
    int t = toks[idx]; yylval = vals[idx]; idx++;
    return t;
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
