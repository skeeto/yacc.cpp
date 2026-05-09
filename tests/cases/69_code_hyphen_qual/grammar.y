%{
#include <stdio.h>
%}

/* Bison's parse-gram.y uses hyphenated %code qualifiers like
   `%code pre-printer {...}`.  We don't recognize these qualifiers
   semantically, but the lexer must at least stitch "pre-printer"
   so the brace block parses. */

%code pre-printer  {/* user code */}
%code post-printer {/* user code */}

%token T

%%
e: T   { printf("ok\n"); } ;
%%
