%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token A B

%%
input: A B  { printf("ok\n"); } ;
%%
