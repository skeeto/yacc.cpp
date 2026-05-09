%language "c++"

%code requires {
    namespace yy { class parser; }
}
%code {
    extern int yylex(yy::parser::semantic_type*);
}

%{
#include <cstdio>
%}

%token A

%%
/* Right-recursion forces shifts to outpace reduces, so the parser
   stack grows linearly with input length.  With YYINITDEPTH=200 (the
   default emit_constants value), feeding 1024 tokens forces several
   stack-doubling growths -- the test fails to terminate / segfaults
   without proper growth code.  A successful run prints "ok\n". */
list: A
    | A list
    ;
%%
