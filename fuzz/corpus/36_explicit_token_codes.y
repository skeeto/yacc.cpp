%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token X 1000
%token Y 999

%%
input: X Y  { printf("ok\n"); }
     ;
%%
