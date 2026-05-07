%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token IF THEN ELSE STMT

%%
program: stmt  { printf("done\n"); } ;
stmt: STMT
    | IF cond THEN stmt           { printf("if-then\n"); }
    | IF cond THEN stmt ELSE stmt { printf("if-then-else\n"); }
    ;
cond: STMT  ;
%%
