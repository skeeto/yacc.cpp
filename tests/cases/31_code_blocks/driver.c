#include <stdio.h>
#include "grammar.tab.h"
extern YYSTYPE yylval;
int yylex(void) {
    static int d; if (d) return 0; d = 1;
    yylval.ival = 100;
    return NUM;
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) {
    /* call helper_fn just to confirm it exists */
    (void)helper_fn(0);
    return yyparse();
}
