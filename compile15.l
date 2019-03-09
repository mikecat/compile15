%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
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

"{"             return '{';
"}"             return '}';
"("             return '(';
")"             return ')';
";"             return ';';
"*"             return '*';
"="             return '=';

"unsigned"      return UNSIGNED;
"char"          return CHAR;
"short"         return SHORT;
"int"           return INT;

[ \t\n]+        {}

[a-zA-Z_][0-9a-zA-Z_]* {
                           yylval.strval = str_dup(yytext);
                           return IDENTIFIER;
                       }
0[xX][0-9a-fA-F]+[uU]? {
                           yylval.intval = 0;
                           sscanf(yytext + 2, "%" SCNx32, &yylval.intval);
                           char last_char = yytext[strlen(yytext) - 1];
                           if (last_char == 'u' || last_char == 'U') {
                               return UNSIGNED_INTEGER_LITERAL;
                           } else {
                               return INTEGER_LITERAL;
                           }
                       }
0[0-7]*[uU]?           {
                           yylval.intval = 0;
                           if (yytext[1] != '\0') {
                               sscanf(yytext + 1, "%" SCNo32, &yylval.intval);
                           }
                           char last_char = yytext[strlen(yytext) - 1];
                           if (last_char == 'u' || last_char == 'U') {
                               return UNSIGNED_INTEGER_LITERAL;
                           } else {
                               return INTEGER_LITERAL;
                           }
                       }
[1-9][0-9]*[uU]?       {
                           yylval.intval = 0;
                           sscanf(yytext, "%" SCNu32, &yylval.intval);
                           char last_char = yytext[strlen(yytext) - 1];
                           if (last_char == 'u' || last_char == 'U') {
                               return UNSIGNED_INTEGER_LITERAL;
                           } else {
                               return INTEGER_LITERAL;
                           }
                       }

. {
              fprintf(stderr, "invalid token %s\n", yytext);
              exit(1);
          }

%%

