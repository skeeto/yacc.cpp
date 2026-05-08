%{
#include <stdio.h>
%}

%token IF THEN ELSE STMT

%%
program: stmt   { printf("done\n"); } ;
stmt: STMT
    | IF cond THEN stmt
    | IF cond THEN stmt ELSE stmt
    ;
cond: STMT  ;
%%
