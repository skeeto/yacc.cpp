%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
int depth;
%}

%token NUMBER

%%
program: list   { printf("done depth=%d\n", depth); } ;
list: /* empty */
    | list balanced
    ;
balanced: NUMBER
        | '(' { depth++; } list ')' { depth--; printf("group depth=%d\n", depth); }
        ;
%%
