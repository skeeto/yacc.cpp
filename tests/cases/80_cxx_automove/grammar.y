%language "c++"
%define api.value.type variant
%define api.value.automove

%code requires {
    #include <memory>
    #include <string>
    namespace yy { class parser; }
}
%code {
    extern int yylex(yy::parser::semantic_type*);
}

%{
#include <cstdio>
%}

%token <std::unique_ptr<int>> PTR

%type <std::unique_ptr<int>> expr

%%
prog: expr   { std::printf("ptr=%d\n", *$1); }
    ;
/* automove turns $1 into std::move($1), so unique_ptr can move from the
   value stack into the LHS slot.  Without automove this would fail to
   compile. */
expr: PTR    { $$ = $1; }
    ;
%%
