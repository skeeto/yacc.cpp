%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
int helper(int x);
%}

%token NUMBER

%%
input: NUMBER  { printf("h(%d)=%d\n", $1, helper($1)); } ;
%%

int helper(int x) { return x * 2; }
