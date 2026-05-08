#include <stdio.h>
#include "grammar.tab.h"
static int idx;
static const int toks[] = { STMT, 0 };
int yylex(void) { return toks[idx++]; }
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
