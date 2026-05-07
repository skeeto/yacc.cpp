%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token A B END

%%
program: A B END     { printf("ok\n"); YYACCEPT; }
       | A B         { printf("nope\n"); YYABORT; }
       ;
%%
