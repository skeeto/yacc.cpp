%{
#include <stdio.h>
%}

%define parse.error verbose
%define parse.lac full

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
