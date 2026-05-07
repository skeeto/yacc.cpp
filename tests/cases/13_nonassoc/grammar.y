%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token NUMBER
%nonassoc '<' '>' '='

%%
input: /* empty */ | input line ;
line: expr '\n'  { printf("=> %d\n", $1); }
    | error '\n' { printf("err\n"); yyerrok; }
    ;
expr: NUMBER       { $$ = $1; }
    | expr '<' expr  { $$ = $1 < $3; }
    | expr '>' expr  { $$ = $1 > $3; }
    | expr '=' expr  { $$ = $1 == $3; }
    ;
%%
