%code requires {
/* Custom location type that still has the four fields the default
 * YYLLOC_DEFAULT macro expects, plus an extra one. */
typedef struct MyLoc {
    int first_line, first_column;
    int last_line,  last_column;
    int extra;
} MyLoc;
}

%{
#include <stdio.h>
%}

%locations
%define api.location.type {MyLoc}
%token NUMBER

%%
input: /* empty */ | input line ;
line: NUMBER '\n'  {
    printf("got %d at %d:%d extra=%d\n",
        $1, @1.first_line, @1.first_column, @1.extra);
} ;
%%
