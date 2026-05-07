%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token T

%%
/* Comment with UTF-8: αβγ δεζ */
input: T  { printf("UTF-8: αβγ\n"); }
     ;
%%
