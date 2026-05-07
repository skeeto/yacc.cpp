%{
#include <stdio.h>
#include <stdlib.h>
int yylex(void);
void yyerror(const char *s);
%}

%expect 0

%union {
    int ival;
    double dval;
}

%token <ival> INT
%token <dval> FLOAT

%type <ival> int_expr
%type <dval> float_expr

%%
input: /* empty */ | input line ;
line: int_expr     { printf("int %d\n", $1); }
    | float_expr   { printf("float %g\n", $1); }
    ;
int_expr: INT  { $$ = $1; } ;
float_expr: FLOAT  { $$ = $1; } ;
%%
