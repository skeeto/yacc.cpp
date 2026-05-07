%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
int sum;
%}

%token NUMBER

%%
list: /* empty */
    | list NUMBER  { sum += $2; printf("got %d sum=%d\n", $2, sum); }
    ;
%%
