%{
#include <stdio.h>
#include <stdlib.h>
#include "ast.h"
#include "util.h"

// avoid implicit declaration warnings
int yyerror(const char* str);
int yylex();

// もとのchainは開放する
ast_node* ast_chain_to_array(ast_chain_node* chain) {
	size_t count = 0;
	ast_chain_node* chain_ptr = chain;
	// 要素数を求める
	while (chain_ptr != NULL) {
		count++;
		chain_ptr = chain_ptr->next;
	}
	// 要素をarrayノードに格納する
	// ついでにchainを開放する
	ast_node* array = malloc_check(sizeof(ast_node));
	array->kind = NODE_ARRAY;
	array->d.array.num = count;
	array->d.array.nodes = malloc_check(sizeof(ast_node) * count);
	chain_ptr = chain;
	for (size_t i = 0; i < count; i++) {
		array->d.array.nodes[i] = chain_ptr->node;
		ast_chain_node* chain_next = chain_ptr->next;
		free(chain_ptr);
		chain_ptr = chain_next;
	}
	return array;
}

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
		{
			$$ = malloc_check(sizeof(ast_chain_node));
			$$->node = $1;
			$$->next = NULL;
		}
	| top_element top_elements
		{
			$$ = malloc_check(sizeof(ast_chain_node));
			$$->node = $1;
			$$->next = $2;
		}
	;

top_element
	: func_def
	;

func_def
	: type IDENTIFIER '(' ')' block
		{
			$$ = malloc_check(sizeof(ast_node));
			$$->kind = NODE_FUNC_DEF;
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
			$$ = malloc_check(sizeof(ast_node));
			$$->kind = NODE_ARRAY;
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
