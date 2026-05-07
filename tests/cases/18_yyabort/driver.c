#include <stdio.h>
#include "grammar.tab.h"
static int idx;
int yylex(void) {
    static const int t[] = { A, B, END, 0 };
    return t[idx++];
}
void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) {
    int rc = yyparse();
    printf("rc=%d\n", rc);
    return 0;
}
