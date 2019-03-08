%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // isatty()
#include "ast.h"
#include "compile15_parse.h"
#include "util.h"

int fileno(FILE*); // avoid implicit declaration warning on MinGW

char* str_dup(const char* str) {
	size_t len = strlen(str);
	char* ret = malloc_check(len + 1);
	return strcpy(ret, str);
}

int yywrap(void) {
	return 1;
}
%}
%%

"{"       return '{';
"}"       return '}';
"("       return '(';
")"       return ')';

"int"     return INT;

[ \t\n]+  {}

[a-zA-Z_][0-9a-zA-Z_]* {
                           yylval.strval = str_dup(yytext);
                           return IDENTIFIER;
                       }

. {
              fprintf(stderr, "invalid token %s\n", yytext);
              exit(1);
          }

%%

