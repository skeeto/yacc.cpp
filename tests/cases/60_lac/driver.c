#include <stdio.h>
#include "grammar.tab.h"
extern YYSTYPE yylval;
static int idx;
int yylex(void) {
    static const int t[] = { IF, NUMBER, NUMBER, 0 };
    yylval = idx;
    return t[idx++];
}
void yyerror(const char *s) { printf("ERR: %s\n", s); }
int main(void) { yyparse(); return 0; }
