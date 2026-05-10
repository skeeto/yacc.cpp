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

%token A B SEMI

%%
prog: stmts ;
stmts: stmt
     | stmts stmt
     ;
/* `stmt: error SEMI` is the classic bison recovery pattern: when the
   parser finds a syntax error mid-statement, it pops the stack
   looking for a state that accepts `error`, then synchronizes by
   skipping ahead to SEMI. */
stmt: A B SEMI    { std::printf("stmt\n"); }
    | error SEMI  { std::printf("recovered\n"); }
    ;
%%
