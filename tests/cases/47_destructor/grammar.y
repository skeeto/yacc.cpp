%code requires {
#include <stdlib.h>
#include <string.h>
}

%{
#include <stdio.h>
%}

%union {
    char *sval;
}

%token <sval> ID
%token END BAD

%destructor { printf("free %s\n", $$); free($$); } <sval>

%%
input: ID ID END     { printf("ok %s %s\n", $1, $2); free($1); free($2); }
     | error END     { printf("recovered\n"); yyerrok; }
     ;
%%
