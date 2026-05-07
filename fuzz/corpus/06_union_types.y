%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int yylex(void);
void yyerror(const char *s);
%}

%union {
    int ival;
    char *sval;
}

%token <ival> INT
%token <sval> STR
%type  <ival> expr

%%
input: /* empty */ | input line ;
line: expr '\n'    { printf("int: %d\n", $1); }
    | STR '\n'     { printf("str: %s\n", $1); free($1); }
    ;
expr: INT          { $$ = $1; }
    | expr '+' INT { $$ = $1 + $3; }
    ;
%%
