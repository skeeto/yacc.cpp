%language "c++"

%code requires {
    namespace yy { class parser; }
}
%code {
    extern int yylex(yy::parser::semantic_type*);
}

%parse-param {int* out}

%{
#include <cstdio>
%}

%token NUM

%%
prog: NUM   { *out = $1 * 10; std::printf("ok\n"); } ;
%%
