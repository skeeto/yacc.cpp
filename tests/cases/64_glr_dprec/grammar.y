%{
#include <stdio.h>
%}

%glr-parser

%token A

%%
program: x   { printf("dprec=%d\n", $1); } ;
x: A %dprec 1 { $$ = 1; }
 | A %dprec 2 { $$ = 2; }
 ;
%%
