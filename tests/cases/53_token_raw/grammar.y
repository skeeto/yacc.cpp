%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%define api.token.raw

%token A B

%%
input: A B  { printf("ok A=%d B=%d\n", A, B); } ;
%%
