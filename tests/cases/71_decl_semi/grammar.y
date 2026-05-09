%{
#include <stdio.h>
%}

/* Bison treats stray ';' between declarations as an empty declaration.
   parse-gram.y has lines like:
       %code requires { ... };
   where the trailing semicolon is not part of the brace block. */

%token T;
%type <int> e;
%union { int n; };

%%
e: T   { printf("ok\n"); } ;
%%
