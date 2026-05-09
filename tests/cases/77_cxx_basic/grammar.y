%language "c++"

%code requires {
    // Forward-declare yylex with bison's preferred prototype so both
    // generators see the same free-function signature.
    namespace yy { class parser; }
}
%code {
    extern int yylex(yy::parser::semantic_type *);
}

%{
#include <cstdio>
%}

%token NUM

%%
prog: expr   ;
expr: NUM        { std::printf("got=%d\n", $1); }
    | expr NUM   { std::printf("more=%d\n", $2); }
    ;
%%
