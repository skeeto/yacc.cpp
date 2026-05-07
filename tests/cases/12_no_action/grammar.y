%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token NUMBER

%%
input: /* empty */ | input line ;
line: expr '\n'  { printf("got %d\n", $1); }
    ;
/* No semantic action: default $$ = $1 */
expr: NUMBER
    ;
%%
