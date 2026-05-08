%code requires {
typedef struct LexState { int counter; } LexState;
}

%{
#include <stdio.h>
%}

%define api.pure full
%lex-param   {LexState *ls}
%parse-param {LexState *ls}

%token NUMBER

%%
input: NUMBER NUMBER NUMBER  {
    printf("counter=%d\n", ls->counter);
};
%%
