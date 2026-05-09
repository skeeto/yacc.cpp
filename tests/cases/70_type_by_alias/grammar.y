%{
#include <stdio.h>
%}

/* Bison's parse-gram.y references tokens by string-literal alias
   in %type:  %type <char*> "{...}" EPILOGUE STRING
   The token is declared without a tag, then %type assigns one. */

%union { int n; }

%token EPILOGUE  "epilogue"
%token PLUS      "+"

%type <n> "+" EPILOGUE

%%
e: PLUS EPILOGUE  { printf("ok %d\n", $1 + $2); }
 ;
%%
