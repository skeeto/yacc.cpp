#include <stdio.h>
#include "grammar.tab.h"
extern YYSTYPE yylval;
int yylex(void) { static int d; if (d) return 0; d = 1; yylval = 7; return NUMBER; }
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
