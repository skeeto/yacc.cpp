%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token A 300
%token B 301
%token C

%%
input: A B C  { printf("a=%d b=%d c-gt-b=%d\n", A, B, C > B); }
     ;
%%
