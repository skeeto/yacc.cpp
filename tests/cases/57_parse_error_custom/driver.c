#include <stdio.h>
#include "grammar.tab.h"
extern YYSTYPE yylval;

static int idx;
int yylex(void) {
    /* IF NUMBER NUMBER -- after IF NUMBER the parser expects THEN
     * but gets NUMBER, triggering a syntax error reported via
     * yyreport_syntax_error (defined in the grammar's epilogue). */
    static const int t[] = { IF, NUMBER, NUMBER, 0 };
    yylval = idx;
    return t[idx++];
}

void yyerror(const char *s) { (void)s; }
int main(void) { yyparse(); return 0; }
