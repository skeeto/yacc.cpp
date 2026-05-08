%{
#include <stdio.h>
%}

%glr-parser
%parse-param {int *out}

%token A

%%
program: x   { *out = $1; printf("got=%d\n", *out); } ;
x: A { $$ = 42; } ;
%%
