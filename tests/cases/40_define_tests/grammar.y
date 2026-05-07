%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%define parse.error verbose
%define parse.lac full
%define lr.type lalr
%define lr.default-reduction most

%token T

%%
input: T   { printf("ok\n"); }
     ;
%%
