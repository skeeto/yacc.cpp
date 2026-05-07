%require "3.0"
%language "c"

%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token T

%%
input: T  { printf("ok\n"); } ;
%%
