#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "grammar.tab.h"

extern YYSTYPE yylval;
static int idx;

static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = (char*)malloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

/* Feed: ID("x") ID("y") BAD END.
 * BAD triggers a syntax error when the parser is expecting END after
 * the two IDs.  Recovery pops the two ID values, firing the <sval>
 * destructor on each ("free y", "free x"), and the `error END`
 * alternative completes the parse.
 */
int yylex(void) {
    static const int seq[] = {ID, ID, BAD, END, 0};
    static const char *names[] = {"x", "y", NULL, NULL, NULL};
    int t = seq[idx];
    if (t == ID) yylval.sval = xstrdup(names[idx]);
    idx++;
    return t;
}

void yyerror(const char *s) { printf("err: %s\n", s); }
int main(void) { yyparse(); return 0; }
