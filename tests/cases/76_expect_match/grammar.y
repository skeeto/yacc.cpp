%{
#include <stdio.h>
%}

/* Bison suppresses the conflict warning when %expect N matches the
   actual count.  Octave declares %expect 9 with exactly 9 conflicts;
   if our generator nags anyway, the tooling treats the warning as a
   build failure. */

%expect 1

%token IF THEN ELSE STMT

%%
program: stmt   { printf("done\n"); } ;
stmt: STMT
    | IF stmt
    | IF stmt ELSE stmt
    ;
%%
