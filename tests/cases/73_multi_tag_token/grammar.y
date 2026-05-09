%{
#include <stdio.h>
%}

/* Perl's perly.y uses multiple <tag> blocks within a single
   precedence declaration, e.g.:
       %left <ival> OROP <pval> PLUGIN_LOGICAL_OR_LOW_OP
   Each tag applies to symbols that follow it, until the next tag. */

%union { int ival; char *pval; }

%left <ival> A B <pval> C D

%token <ival> X

%%
e: X   { printf("ok %d\n", $1); }
 ;
%%
