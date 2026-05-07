%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
int count;
%}

%token NUMBER

%%
input: list  { printf("count=%d\n", count); } ;
/* Right-recursive list forces deep stack growth */
list: /* empty */
    | NUMBER list  { count++; }
    ;
%%
