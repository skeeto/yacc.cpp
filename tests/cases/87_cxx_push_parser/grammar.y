%language "c++"
%define api.push-pull push

%code requires {
    namespace yy { class parser; }
}

%{
#include <cstdio>
%}

%token NUM PLUS

%%
expr: NUM             { std::printf("got=%d\n", $1); }
    | expr PLUS NUM   { std::printf("sum=%d\n", $1 + $3); }
    ;
%%
