#include <stdio.h>
#include "grammar.tab.h"

YYSTYPE my_merger(YYSTYPE a, YYSTYPE b) { return a + b; }

static int idx;
int yylex(void) {
    static const int t[] = { A, 0 };
    return t[idx++];
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return yyparse(); }
