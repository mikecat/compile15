%{
#include <stdio.h>

// avoid implicit declaration warnings
int yyerror(const char* str);
int yylex();

%}
%union {
	char* strval;
}
%token INT
%token <strval> IDENTIFIER
%%
func_def
	: type IDENTIFIER '(' ')' block

type
	: INT

block
	: '{' '}'
%%
int yyerror(const char* str) {
	extern char *yytext;
	fprintf(stderr, "parse error: %s near %s\n", str, yytext);
	return 0;
}

int main(void) {
	if (yyparse()) return 1;
	return 0;
}
