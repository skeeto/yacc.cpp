%{
#include <stdio.h>
%}
%token A B
%%
input: A B  { printf("ok\n"); } ;
%%
