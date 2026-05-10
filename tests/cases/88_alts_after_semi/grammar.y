%{
#include <stdio.h>
%}

/* Bison allows `;` + `|` to continue adding alternatives to the
   same rule.  rl78-parse.y from binutils uses this to break up its
   long instruction-encoding rules. */

%token A B C

%%
prog: foo  { printf("ok\n"); } ;
foo: A
   | B
;
   | C
;
%%
