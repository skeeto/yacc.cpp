%{
#include <stdio.h>
int yylex(void);
void yyerror(const char *s);
%}

%locations
%token A B C

%%
program: triple[t] {
    printf("triple at %d:%d-%d:%d\n",
           @t.first_line, @t.first_column,
           @t.last_line,  @t.last_column);
} ;
triple: A[a] B[b] C[c] {
    printf("@a=%d:%d @b=%d:%d @c=%d:%d\n",
           @a.first_line, @a.first_column,
           @b.first_line, @b.first_column,
           @c.first_line, @c.first_column);
} ;
%%
