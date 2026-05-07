%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token NUMBER

%%
input: /* empty */ | input line ;
line: '\012'  { printf("nl-oct\n"); }   /* \012 is newline */
    | NUMBER '\012'  { printf("num=%d\n", $1); }
    ;
%%
