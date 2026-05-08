%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%locations
%token NUMBER
%left '+'

%%
input: /* empty */ | input line ;
line: expr '\n'  {
    printf("expr=%d at %d:%d-%d:%d\n", $1,
           @1.first_line, @1.first_column,
           @1.last_line,  @1.last_column);
} ;
expr: NUMBER          { $$ = $1; }
    | expr '+' expr   {
        $$ = $1 + $3;
        printf("sum at %d:%d-%d:%d\n",
               @$.first_line, @$.first_column,
               @$.last_line,  @$.last_column);
    }
    ;
%%
