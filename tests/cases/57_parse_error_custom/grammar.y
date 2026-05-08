%{
#include <stdio.h>
%}

%define parse.error custom

%token IF "if"
%token THEN "then"
%token ELSE "else"
%token NUMBER

%%
program: stmt   { printf("ok\n"); } ;
stmt: IF NUMBER THEN NUMBER
    | IF NUMBER THEN NUMBER ELSE NUMBER
    ;
%%

/* yyreport_syntax_error lives in the .y epilogue so the parser's
 * yypcontext_t typedef and yypcontext_* helpers (defined in the same
 * TU) are visible without exporting them through the header. */
int yyreport_syntax_error(const yypcontext_t *ctx) {
    yysymbol_kind_t expected[8];
    int n = yypcontext_expected_tokens(ctx, expected, 8);
    yysymbol_kind_t got = yypcontext_token(ctx);
    printf("CUSTOM: got=%s, expected:", yysymbol_name(got));
    for (int i = 0; i < n; i++) printf(" %s", yysymbol_name(expected[i]));
    printf("\n");
    return 0;
}
