#include <stdio.h>
#include "grammar.tab.h"
extern YYSTYPE yylval;
static int idx;
static const int toks[]    = { INT, FLOAT, INT, 0 };
int yylex(void) {
    static const long ivals[]  = { 5,    0,     -3,  0 };
    static const double dvals[] = { 0.0,  3.14,  0.0, 0.0 };
    int t = toks[idx];
    if (t == INT) yylval.ival = (int)ivals[idx];
    else if (t == FLOAT) yylval.dval = dvals[idx];
    idx++;
    return t;
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
