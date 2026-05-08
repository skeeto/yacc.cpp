%{
#include <stdio.h>
%}

%define lr.type canonical-lr

%token NUMBER

%%
input: /* empty */ | input line ;
line: expr '\n'  { printf("=%d\n", $1); }
    ;
expr: NUMBER          { $$ = $1; }
    | expr '+' NUMBER { $$ = $1 + $3; }
    ;
%%
