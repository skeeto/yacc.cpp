%code requires {
    /* %code requires goes in header before YYSTYPE */
    typedef struct point { int x, y; } point_t;
}

%code provides {
    /* %code provides goes in header after YYSTYPE */
    int helper_fn(int x);
}

%code top {
    /* %code top goes at the very top of the source */
}

%code {
    /* unqualified %code goes in source */
    static int helper_static = 42;
}

%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%union {
    int ival;
    point_t pt;
}

%token <ival> NUM

%%
input: NUM { printf("%d helper=%d\n", $1, helper_static); }
     ;
%%

int helper_fn(int x) { return x; }
