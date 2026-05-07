#include <stdio.h>
#include "grammar.tab.h"
/* Test "if S then if S then S else S" - dangling else binds to inner. */
static int idx;
static int toks[16];
int yylex(void) { return toks[idx++]; }
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) {
    int i = 0;
    toks[i++] = IF;
    toks[i++] = STMT;
    toks[i++] = THEN;
    toks[i++] = IF;
    toks[i++] = STMT;
    toks[i++] = THEN;
    toks[i++] = STMT;
    toks[i++] = ELSE;
    toks[i++] = STMT;
    toks[i++] = 0;
    return yyparse();
}
