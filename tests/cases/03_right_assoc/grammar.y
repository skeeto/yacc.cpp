%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%token NUMBER
%left '+'
%right '^'

%%
input: /* empty */ | input line ;
line: '\n' | expr '\n'  { printf("%d\n", $1); } ;
expr: NUMBER         { $$ = $1; }
    | expr '+' expr  { $$ = $1 + $3; }
    | expr '^' expr  {
        int b = $1, e = $3, r = 1;
        while (e-- > 0) r *= b;
        $$ = r;
      }
    ;
%%
