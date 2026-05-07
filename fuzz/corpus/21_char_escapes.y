/* Block comments are stripped. */
%{
#include <stdio.h>
// Line comment in prologue
int yylex(void);
void yyerror(const char *s);
%}

%token NUMBER

/* Comments between sections */
%%
input: /* empty */ | input line ;
line: '\n'  { printf("nl\n"); }
    | '\t' '\n'  { printf("tab\n"); }
    | NUMBER '\n'  { printf("num=%d\n", $1); }
    ;
%%
