%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token NUMBER

%%
input: NUMBER  { printf("got=%d\n", $1); } ;
%%
