#include <stdio.h>
#include "grammar.tab.h"
int yylex(void) { static int d; if (d) return 0; d = 1; return T; }
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
