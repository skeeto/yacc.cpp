%language "c++"
%define parse.error custom

%code requires {
    namespace yy { class parser; }
}
%code {
    extern int yylex(yy::parser::semantic_type*);
}

%{
#include <cstdio>
%}

%token A B

%%
prog: A B   { std::printf("ok\n"); } ;
%%
