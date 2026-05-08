%{
#include <stdio.h>
%}

%glr-parser

%token A

%%
program: x   { printf("done\n"); } ;
x: A   { printf("x=A\n"); }
 ;
%%
