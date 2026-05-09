%{
#include <stdio.h>
%}

/* Bison's parse-gram.y declares tokens with translatable aliases:
   %token NAME _("string")
   The _(...) wrapper marks the alias for gettext at compile time.
   Bison treats _("...") as equivalent to a bare "..." for token-name
   purposes. */

%token PLUS  _("plus sign")
%token MINUS "-"

%%
e: PLUS   { printf("plus\n"); }
 | MINUS  { printf("minus\n"); }
 ;
%%
