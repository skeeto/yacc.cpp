%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%define api.token.prefix {TOK_}

%token NUMBER

%%
input: NUMBER  { printf("got=%d\n", $1); } ;
%%
