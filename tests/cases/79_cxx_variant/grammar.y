%language "c++"
%define api.value.type variant

%code requires {
    #include <string>
    namespace yy { class parser; }
}
%code {
    extern int yylex(yy::parser::semantic_type*);
}

%{
#include <cstdio>
%}

%token <int>          NUM
%token <std::string>  STR

%type <int> expr

%%
prog: expr   { std::printf("answer=%d\n", $1); }
    ;
expr: NUM            { $$ = $1; }
    | NUM '+' NUM    { $$ = $1 + $3; }
    | STR            { $$ = (int)$1.size(); }
    ;
%%
