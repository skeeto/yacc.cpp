#include <stdio.h>
#include "grammar.tab.h"

void yyerror(const char *s) { fprintf(stderr, "%s\n", s); }

/* Push-parser usage: caller drives the parser by feeding tokens one
 * at a time.  yypush_parse returns YYPUSH_MORE when it needs another
 * token, 0 on accept, >1 on error. */
int main(void) {
    yypstate *ps = yypstate_new();
    int rc = YYPUSH_MORE;

    int c;
    YYSTYPE val;
    while (rc == YYPUSH_MORE) {
        do { c = getchar(); } while (c == ' ' || c == '\t');
        int tok;
        if (c == EOF) { tok = 0; val = 0; }
        else if (c >= '0' && c <= '9') {
            int v = c - '0';
            for (;;) {
                int d = getchar();
                if (d < '0' || d > '9') { if (d != EOF) ungetc(d, stdin); break; }
                v = v * 10 + (d - '0');
            }
            val = v; tok = NUMBER;
        } else { val = 0; tok = c; }
        rc = yypush_parse(ps, tok, &val);
    }

    yypstate_delete(ps);
    return rc;
}
