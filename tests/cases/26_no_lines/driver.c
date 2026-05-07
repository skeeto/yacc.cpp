#include <stdio.h>
#include "grammar.tab.h"
extern YYSTYPE yylval;
int yylex(void) {
    static int done; if (done) return 0; done = 1;
    yylval = 99;
    return NUMBER;
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
