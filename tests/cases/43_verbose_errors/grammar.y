%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%define parse.error verbose
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
