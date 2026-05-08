%{
#include <stdio.h>
%}

%define api.push-pull push
%define api.pure full

%token NUMBER

%%
input: /* empty */ | input line ;
line: NUMBER '\n'  { printf("got %d\n", $1); } ;
%%
