%language "c++"
%locations

%code requires {
    namespace yy { class parser; }
}
%code {
    extern int yylex(yy::parser::semantic_type*, yy::parser::location_type*);
}

%{
#include <cstdio>
%}

%token NUM

%%
prog: expr   ;
expr: NUM        { std::printf("got=%d at %d.%d-%d.%d\n",
                               $1,
                               @1.begin.line, @1.begin.column,
                               @1.end.line,   @1.end.column); }
    ;
%%
