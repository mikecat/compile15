%{
#include <stdio.h>
#include "ast.h"
#include "util.h"

// avoid implicit declaration warnings
int yyerror(const char* str);
int yylex();

ast_node* top_ast;

%}
%union {
	char* strval;
	ast_node* node;
	type_kind type;
}
%token <strval> IDENTIFIER
%token <type> INT
%type <type> type
%type <node> top func_def block

%start top
%%
top
	: func_def
		{ top_ast = $1; }
	;

func_def
	: type IDENTIFIER '(' ')' block
		{
			ast_node* node = malloc_check(sizeof(ast_node));
			node->kind = NODE_FUNC_DEF;
			node->d.func_def.return_type = $1;
			node->d.func_def.name = $2;
			node->d.func_def.body = $5;
			$$ = node;
		}
	;

type
	: INT
		{ $$ = TYPE_INT; }
	;

block
	: '{' '}'
		{ $$ = NULL; }
	;
%%
int yyerror(const char* str) {
	extern char *yytext;
	fprintf(stderr, "parse error: %s near %s\n", str, yytext);
	return 0;
}

ast_node* build_ast(FILE* fp) {
	extern FILE* yyin;
	yyin = fp;
	if (yyparse()) return NULL;
	return top_ast;
}
