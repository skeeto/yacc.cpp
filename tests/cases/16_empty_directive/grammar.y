%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token A B

%%
program: list  { printf("done\n"); } ;
list: %empty   { printf("empty\n"); }
    | list A   { printf("A\n"); }
    | list B   { printf("B\n"); }
    ;
%%
