/* Two parsers in one binary, each with its own api.prefix.
 * Each .h gets its own yyparse / yylex / yylval / YYSTYPE renames so
 * the two parsers don't collide at link time.  Each TU here includes
 * only its own header (driver.c -> grammar_a.tab.h, driver_b.c ->
 * grammar_b.tab.h); main() calls into both. */
#include <stdio.h>
#include "grammar_a.tab.h"

extern int b_parse(void);

static int a_seq_idx = 0;
int a_lex(void) {
    if (a_seq_idx++ >= 1) return 0;
    return NUMBER;
}
void a_error(const char *s) { fprintf(stderr, "a: %s\n", s); }

int main(void) {
    int ra = a_parse();
    int rb = b_parse();
    printf("a=%d b=%d\n", ra, rb);
    return ra | rb;
}
