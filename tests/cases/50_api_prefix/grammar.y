%{
#include <stdio.h>
int  foo_lex(void);
void foo_error(const char *s);
%}

%define api.prefix {foo_}

%token NUMBER

%%
input: NUMBER  { printf("got=%d\n", $1); } ;
%%
