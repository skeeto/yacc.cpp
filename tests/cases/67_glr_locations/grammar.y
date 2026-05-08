%{
#include <stdio.h>
%}

%glr-parser
%locations

%token A B

%%
program: x   { printf("loc: %d.%d-%d.%d\n",
                      @1.first_line, @1.first_column,
                      @1.last_line,  @1.last_column); } ;
x: A B   { printf("x produced\n"); } ;
%%
