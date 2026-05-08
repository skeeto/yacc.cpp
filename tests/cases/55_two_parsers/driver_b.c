#include <stdio.h>
#include "grammar_b.tab.h"

static int b_seq_idx = 0;
int b_lex(void) {
    if (b_seq_idx++ >= 1) return 0;
    return WORD;
}
void b_error(const char *s) { fprintf(stderr, "b: %s\n", s); }
