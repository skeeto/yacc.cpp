%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
extern int yynerrs;
%}

%token A B C

%%
program: A B C   { printf("ok nerrs=%d\n", yynerrs); }
       | A error C  { printf("recovered nerrs=%d\n", yynerrs); }
       ;
%%
