%{
#include <stdio.h>
%}

/* Bison's own parse-gram.y interleaves %type / %printer / %destructor
   declarations with grammar rules.  Such a directive must end with `;`
   to terminate it inside the rules section.  We accept this even
   though the type-tag is captured but not retroactively applied. */

%union { int n; }

%token <n> A B

%%
e: A B   { printf("got %d\n", $1 + $2); } ;

%type <n> e;
%%
