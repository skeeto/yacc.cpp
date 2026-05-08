#include <stdio.h>
#include "grammar.tab.h"

int yylex(YYSTYPE *lvalp, LexState *ls) {
    if (ls->counter >= 3) return 0;
    *lvalp = ls->counter;
    ls->counter++;
    return NUMBER;
}

void yyerror(LexState *ls, const char *s) { (void)ls; fprintf(stderr, "%s\n", s); }
int main(void) {
    LexState s = { 0 };
    return yyparse(&s);
}
