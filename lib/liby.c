/* liby.c - the one-screen yacc compatibility library.
 *
 * Bison and traditional yacc let you write a grammar without supplying
 * main() or yyerror() and link the missing symbols from -ly (liby.a).
 * The library is exactly what's below: a default main() that just runs
 * the parser, and a default yyerror() that prints to stderr.
 *
 * yacc.cpp's generated parsers already include a __attribute__((weak))
 * yyerror() body, so users typically don't need this library.  Link with
 * -ly only if your build system explicitly does so for Bison/yacc compat,
 * or if you want the default main().
 *
 * This library has no header file: yyparse() is declared in the parser's
 * own generated header, and yyerror() is conventional.
 */

#include <stdio.h>

extern int yyparse(void);

int main(void) {
    return yyparse();
}

void yyerror(const char *msg) {
    (void)fprintf(stderr, "%s\n", msg);
}
