%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token PLUS MINUS NUMBER

%%
input: NUMBER PLUS NUMBER  { printf("ok\n"); }
     ;
%%
