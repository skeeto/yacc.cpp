%language "c++"
%define api.value.type variant
%define api.token.constructor

%code requires {
    #include <string>
    namespace yy { class parser; }
}
%code {
    extern yy::parser::symbol_type yylex();
}

%{
#include <cstdio>
%}

%token <int>          NUM
%token                EOL
%type  <int>          expr

%%
prog: expr EOL   { std::printf("got=%d\n", $1); } ;
expr: NUM        { $$ = $1; }
    ;
%%
