#include <stdio.h>
#include "grammar.tab.h"
extern YYSTYPE yylval;
static int idx;
int yylex(void) {
    if (idx++ >= 500) return 0;  /* triggers stack growth past 200 */
    yylval = idx;
    return NUMBER;
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
