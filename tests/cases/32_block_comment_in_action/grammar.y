%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token T

%%
input: T  { /* block comment
                spanning multiple
                lines */
            // line comment
            printf("ok\n");
          }
     ;
%%
