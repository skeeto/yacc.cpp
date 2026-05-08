%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
int counter;
%}

%initial-action {
    counter = 100;
    printf("init\n");
}

%token T

%%
input: T  { printf("counter=%d\n", counter); }
     ;
%%
