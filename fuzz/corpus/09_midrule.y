%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token A B C

%%
program: A { printf("seen A\n"); } B { printf("seen B\n"); } C { printf("seen C\n"); }
       ;
%%
