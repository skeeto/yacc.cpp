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
#include <iostream>
#include <sstream>
%}

%token NUM

%%
prog: NUM   { std::ostringstream o; o << @1;
              std::printf("loc=%s\n", o.str().c_str()); } ;
%%
