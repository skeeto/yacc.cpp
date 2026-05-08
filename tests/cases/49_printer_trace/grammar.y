%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%define parse.trace

%union { int ival; }
%token <ival> NUM

%printer { fprintf(yyo, "{%d}", $$); } <ival>

%%
input: NUM NUM    { printf("got %d %d\n", $1, $2); }
     ;
%%
