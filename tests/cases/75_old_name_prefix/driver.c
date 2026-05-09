#include <stdio.h>
#include "grammar.tab.h"
static int idx;
int my_lex(void) {
    static const int t[] = { T, 0 };
    return t[idx++];
}
void my_error(const char *s) { fprintf(stderr, "%s\n", s); }
int main(void) { return my_parse(); }
