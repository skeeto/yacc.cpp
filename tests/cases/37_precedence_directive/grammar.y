%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token NUMBER
%left '+'
%precedence UMINUS

%%
input: /* empty */ | input line ;
line: expr '\n'  { printf("=%d\n", $1); } ;
expr: NUMBER     { $$ = $1; }
    | expr '+' expr  { $$ = $1 + $3; }
    | '-' expr %prec UMINUS  { $$ = -$2; }
    ;
%%
