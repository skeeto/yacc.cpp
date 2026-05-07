%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token A B
%start program

%%
chunk: /* unused */ ;
program: A B  { printf("matched A B\n"); }
       ;
%%
