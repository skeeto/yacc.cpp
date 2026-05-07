%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token NUMBER
%left '+'
%left '*'

%%
input: /* empty */ | input line ;
line: expr '\n'  { printf("%d\n", $1); }
    ;
expr[result]: expr[left] '+' expr[right]   { $result = $left + $right; }
            | expr[left] '*' expr[right]   { $result = $left * $right; }
            | NUMBER                        { $$ = $1; }
            ;
%%
