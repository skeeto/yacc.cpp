%code requires {
typedef struct Ctx { int sum; const char *prefix; } Ctx;
}

%{
#include <stdio.h>
%}

%parse-param {Ctx *ctx}

%token NUMBER

%%
input: /* empty */ | input line ;
line: NUMBER '\n'  { ctx->sum += $1; printf("%s%d\n", ctx->prefix, ctx->sum); } ;
%%
