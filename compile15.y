%{
#include <stdio.h>
#include <stdlib.h>
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
	ast_chain_node* node_chain;
	type_kind type;
}
%token <strval> IDENTIFIER
%token <type> INT
%type <type> type
%type <node> top top_element func_def block
%type <node_chain> top_elements

%start top
%%
top
	: top_elements
		{ $$ = top_ast = ast_chain_to_array($1); }
	;

top_elements
	: top_element
		{ $$ = new_chain_node($1, NULL); }
	| top_element top_elements
		{ $$ = new_chain_node($1, $2); }
	;

top_element
	: func_def
	;

func_def
	: type IDENTIFIER '(' ')' block
		{
			$$ = new_ast_node(NODE_FUNC_DEF);
			$$->d.func_def.return_type = $1;
			$$->d.func_def.name = $2;
			$$->d.func_def.body = $5;
		}
	;

type
	: INT
		{ $$ = TYPE_INT; }
	;

block
	: '{' '}'
		{
			$$ = new_ast_node(NODE_ARRAY);
			$$->d.array.num = 0;
			$$->d.array.nodes = NULL;
		}
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
