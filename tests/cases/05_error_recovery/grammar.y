%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token NUMBER

%%
input: /* empty */ | input line ;
line: stmt '\n'  { printf("ok\n"); }
    | error '\n' { printf("recovered\n"); yyerrok; }
    ;
stmt: NUMBER ';' NUMBER  { printf("pair %d %d\n", $1, $3); }
    ;
%%
