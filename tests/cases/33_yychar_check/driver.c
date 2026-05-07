#include <stdio.h>
#include "grammar.tab.h"
static int idx;
int yylex(void) {
    static const int t[] = { A, '+', C, 0 };  /* '+' is a syntax error after A */
    return t[idx++];
}
void yyerror(const char *s) { (void)s; }
int main(void) { return yyparse(); }
