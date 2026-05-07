%{
#include <stdio.h>
#include <stdlib.h>
int yylex(void);
void yyerror(const char *s);
%}

%token NUMBER
%left '+' '-'
%left '*' '/'
%right UMINUS

%%
input: /* empty */ | input line ;
line: '\n'
    | expr '\n'  { printf("%d\n", $1); }
    ;
expr: NUMBER          { $$ = $1; }
    | expr '+' expr   { $$ = $1 + $3; }
    | expr '-' expr   { $$ = $1 - $3; }
    | expr '*' expr   { $$ = $1 * $3; }
    | expr '/' expr   { $$ = $1 / $3; }
    | '-' expr %prec UMINUS  { $$ = -$2; }
    | '(' expr ')'    { $$ = $2; }
    ;
%%
