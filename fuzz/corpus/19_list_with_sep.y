%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
int total;
%}

%token NUMBER

%%
input: list  { printf("total=%d\n", total); } ;
list: NUMBER         { total += $1; }
    | list ',' NUMBER  { total += $3; }
    ;
%%
