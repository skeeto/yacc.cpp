%{
#include <stdio.h>
#include <stdlib.h>
int yylex(void);
void yyerror(const char *s);
%}

%token IDENT NUMBER LE GE EQ NE
%left  EQ NE
%left  '<' '>' LE GE
%left  '+' '-'
%left  '*' '/' '%'
%right NOT
%nonassoc UMINUS

%%
program: stmt_list  ;
stmt_list: stmt
         | stmt_list ';' stmt
         ;
stmt: IDENT '=' expr  { printf("ASSIGN\n"); }
    | expr            { printf("EXPR=%d\n", $1); }
    ;
expr: NUMBER          { $$ = $1; }
    | IDENT           { $$ = 0; }
    | expr '+' expr   { $$ = $1 + $3; }
    | expr '-' expr   { $$ = $1 - $3; }
    | expr '*' expr   { $$ = $1 * $3; }
    | expr '/' expr   { $$ = $1 / $3; }
    | expr '%' expr   { $$ = $1 % $3; }
    | expr '<' expr   { $$ = $1 < $3; }
    | expr '>' expr   { $$ = $1 > $3; }
    | expr LE expr    { $$ = $1 <= $3; }
    | expr GE expr    { $$ = $1 >= $3; }
    | expr EQ expr    { $$ = $1 == $3; }
    | expr NE expr    { $$ = $1 != $3; }
    | NOT expr        { $$ = !$2; }
    | '-' expr %prec UMINUS  { $$ = -$2; }
    | '(' expr ')'    { $$ = $2; }
    ;
%%
