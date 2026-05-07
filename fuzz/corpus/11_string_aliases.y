%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token IF "if"
%token THEN "then"
%token NUMBER

%%
program: stmt    { printf("done\n"); } ;
stmt: "if" NUMBER "then" NUMBER  { printf("if %d then %d\n", $2, $4); }
    ;
%%
