%{
#include <stdio.h>
%}

/* Perl's perly.y has named mid-rule actions:
       my_var
       { ... }[variable]
       PERLY_PAREN_OPEN ...
   The [name] binds the mid-rule's $$ to a name accessible from later
   $name references in the same alternative. */

%token A B

%%
e: A
   { printf("mid\n"); }[mid_action]
   B    { printf("done\n"); }
 ;
%%
