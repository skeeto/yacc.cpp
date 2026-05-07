%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token NUMBER

%%
input: /* empty */ | input line ;
line: '\x0a'  { printf("hex-nl\n"); }   /* \x0a is newline */
    | NUMBER '\x0a'  { printf("num=%d\n", $1); }
    ;
%%
