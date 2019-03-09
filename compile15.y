%{
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "ast.h"
#include "util.h"

// avoid implicit declaration warnings
int yyerror(const char* str);
int yylex();

ast_node* top_ast;

%}
%union {
	char* strval;
	uint32_t intval;
	ast_node* node;
	ast_chain_node* node_chain;
	type_node* type;
	expression_node* expression;
}
%token <strval> IDENTIFIER
%token <intval> INTEGER_LITERAL UNSIGNED_INTEGER_LITERAL
%token UNSIGNED CHAR SHORT INT
%type <type> type
%type <expression> expression
%type <node> top top_element gvar_def func_def block
%type <node_chain> top_elements

/* 下に行くほど優先順位が高い */
%left ','
%right '?' ':'

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
	: gvar_def
	| func_def
	;

gvar_def
	: type IDENTIFIER ';'
		{
			$$ = new_ast_node(NODE_VAR_DEF);
			$$->d.var_def.type = $1;
			$$->d.var_def.name = $2;
			$$->d.var_def.initializer = NULL;
		}
	| type IDENTIFIER '=' expression ';'
		{
			$$ = new_ast_node(NODE_VAR_DEF);
			$$->d.var_def.type = $1;
			$$->d.var_def.name = $2;
			$$->d.var_def.initializer = $4;
		}
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
	: type '*'
		{ $$ = new_ptr_type($1); }
	| CHAR
		{ $$ = new_prim_type(1, 1); }
	| UNSIGNED CHAR
		{ $$ = new_prim_type(1, 0); }
	| SHORT
		{ $$ = new_prim_type(2, 1); }
	| UNSIGNED SHORT
		{ $$ = new_prim_type(2, 0); }
	| INT
		{ $$ = new_prim_type(4, 1); }
	| UNSIGNED INT
		{ $$ = new_prim_type(4, 0); }
	;

block
	: '{' '}'
		{
			$$ = new_ast_node(NODE_ARRAY);
			$$->d.array.num = 0;
			$$->d.array.nodes = NULL;
		}
	;

expression
	: INTEGER_LITERAL
		{ $$ = new_integer_literal($1, 1); }
	| UNSIGNED_INTEGER_LITERAL
		{ $$ = new_integer_literal($1, 0); }
	| IDENTIFIER
		{ $$ = new_expr_identifier($1); }
	| '(' expression ')'
		{ $$ = new_operator(OP_PARENTHESIS, $2); }
	| expression ',' expression
		{ $$ = new_operator(OP_COMMA, $1, $3); }
	| expression '?' expression ':' expression
		{ $$ = new_operator(OP_COND, $1, $3, $5); }
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
