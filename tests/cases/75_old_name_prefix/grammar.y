%{
#include <stdio.h>
%}

/* PostgreSQL's gram.y uses Bison's deprecated form with `=` and a
   quoted string:
       %name-prefix="base_yy"
   Bison still accepts it (with a -Wdeprecated warning). */

%name-prefix="my_"

%token T

%%
e: T   { printf("ok\n"); } ;
%%
