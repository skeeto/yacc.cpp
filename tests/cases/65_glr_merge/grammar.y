%{
#include <stdio.h>
%}

%glr-parser

%token A

%%
program: x   { printf("merged=%d\n", $1); } ;
x: A %merge<my_merger> { $$ = 10; }
 | A %merge<my_merger> { $$ = 20; }
 ;
%%
