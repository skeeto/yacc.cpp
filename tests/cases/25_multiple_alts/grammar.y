%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token A B C D E F G H

%%
program: list  { printf("done\n"); } ;
list: /* empty */
    | list elem
    ;
elem: A { printf("A\n"); }
    | B { printf("B\n"); }
    | C { printf("C\n"); }
    | D { printf("D\n"); }
    | E { printf("E\n"); }
    | F { printf("F\n"); }
    | G { printf("G\n"); }
    | H { printf("H\n"); }
    ;
%%
