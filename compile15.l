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

%s PP

%%

";"             return ';';
"{"             return '{';
"}"             return '}';
"["             return '[';
"]"             return ']';
"("             return '(';
")"             return ')';
"++"            return INC;
"--"            return DEC;
"&"             return '&';
"*"             return '*';
"+"             return '+';
"-"             return '-';
"~"             return '~';
"!"             return '!';
"sizeof"        return SIZEOF;
"/"             return '/';
"%"             return '%';
"<<"            return SHL;
">>"            return SHR;
"<"             return '<';
">"             return '>';
"<="            return LE;
">="            return GE;
"=="            return EQ;
"!="            return NEQ;
"^"             return '^';
"|"             return '|';
"&&"            return LAND;
"||"            return LOR;
"?"             return '?';
":"             return ':';
"="             return '=';
"*="            return MUL_A;
"/="            return DIV_A;
"%="            return MOD_A;
"+="            return ADD_A;
"-="            return SUB_A;
"<<="           return SHL_A;
">>="           return SHR_A;
"&="            return AND_A;
"^="            return XOR_A;
"|="            return OR_A;
","             return ',';
"#"             {
                    BEGIN(PP);
                    return '#';
                }
<PP>"\n"        {
                    BEGIN(INITIAL);
                    yylloc.first_line++;
                    return '\n';
                }
<PP>"pragma"    return PRAGMA;
<PP>[a-zA-Z_][0-9a-zA-Z_]* {
                               yylval.strval = str_dup(yytext);
                               return IDENTIFIER;
                           }

"void"          return VOID;
"unsigned"      return UNSIGNED;
"char"          return CHAR;
"short"         return SHORT;
"int"           return INT;
"register"      return REGISTER;

"if"            return IF;
"else"          return ELSE;
"do"            return DO;
"while"         return WHILE;
"for"           return FOR;
"goto"          return GOTO;
"continue"      return CONTINUE;
"break"         return BREAK;
"return"        return RETURN;

[ \t]+        {}

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

"\n"                   { yylloc.first_line++; }

. {
              fprintf(stderr, "invalid token %s at line %d\n", yytext, yylloc.first_line);
              exit(1);
          }

%%

