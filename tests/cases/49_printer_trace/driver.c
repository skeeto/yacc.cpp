#include <stdio.h>
#include "grammar.tab.h"
extern YYSTYPE yylval;
extern int yydebug;
static int idx;
int yylex(void) {
    static const int t[] = {NUM, NUM, 0};
    static const int v[] = {7, 11, 0};
    if (t[idx] == 0) return 0;
    yylval.ival = v[idx];
    return t[idx++];
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) {
    yydebug = 1;
    return yyparse();
}
