%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int yylex(void);
void yyerror(const char *s);
int depth;
static void indent(void) { for (int i = 0; i < depth; i++) printf("  "); }
%}

%union {
    long ival;
    char *sval;
}

%token <ival> NUMBER
%token <sval> STRING
%token TRUE FALSE NULL_TOK

%%
value: object | array | string_lit | number_lit | bool_lit | null_lit ;

object: '{' { indent(); printf("{\n"); depth++; } members '}' { depth--; indent(); printf("}\n"); }
      | '{' '}' { indent(); printf("{}\n"); }
      ;

members: pair
       | members ',' pair
       ;

pair: STRING ':' { indent(); printf("\"%s\":\n", $1); depth++; free($1); } value { depth--; }
    ;

array: '[' { indent(); printf("[\n"); depth++; } elements ']' { depth--; indent(); printf("]\n"); }
     | '[' ']' { indent(); printf("[]\n"); }
     ;

elements: value
        | elements ',' value
        ;

string_lit: STRING { indent(); printf("str=\"%s\"\n", $1); free($1); } ;
number_lit: NUMBER { indent(); printf("num=%ld\n", $1); } ;
bool_lit: TRUE     { indent(); printf("true\n"); }
        | FALSE    { indent(); printf("false\n"); }
        ;
null_lit: NULL_TOK { indent(); printf("null\n"); } ;
%%
