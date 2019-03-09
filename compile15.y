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
%token UNSIGNED CHAR SHORT INT REGISTER
%type <type> type
%type <expression> expression
%type <node> top top_element var_def func_def block statement
%type <node_chain> top_elements block_elements

/* 下に行くほど優先順位が高い */
%left ','
%right '?' ':'

%start top
%%
top
	: top_elements
		{ $$ = top_ast = ast_chain_to_array($1, @1.first_line); }
	;

top_elements
	: top_element
		{ $$ = new_chain_node($1, NULL); }
	| top_element top_elements
		{ $$ = new_chain_node($1, $2); }
	;

top_element
	: var_def
	| func_def
	;

var_def
	: type IDENTIFIER ';'
		{
			$$ = new_ast_node(NODE_VAR_DEF, @1.first_line);
			$$->d.var_def.type = $1;
			$$->d.var_def.name = $2;
			$$->d.var_def.is_register = 0;
			$$->d.var_def.initializer = NULL;
		}
	| type IDENTIFIER '=' expression ';'
		{
			$$ = new_ast_node(NODE_VAR_DEF, @1.first_line);
			$$->d.var_def.type = $1;
			$$->d.var_def.name = $2;
			$$->d.var_def.is_register = 0;
			$$->d.var_def.initializer = $4;
		}
	| type IDENTIFIER '[' expression ']' ';'
		{
			if ($4->kind != EXPR_INTEGER_LITERAL || (int)($4->info.value) <= 0) {
				yyerror("unsupported array length");
				YYERROR;
			}
			$$ = new_ast_node(NODE_VAR_DEF, @1.first_line);
			$$->d.var_def.type = new_array_type($4->info.value, $1);
			$$->d.var_def.name = $2;
			$$->d.var_def.is_register = 0;
			$$->d.var_def.initializer = NULL;
		}
	| type IDENTIFIER '[' expression ']' '=' '{' expression '}' ';'
		{
			if ($4->kind != EXPR_INTEGER_LITERAL || (int)($4->info.value) <= 0) {
				yyerror("unsupported array length");
				YYERROR;
			}
			$$ = new_ast_node(NODE_VAR_DEF, @1.first_line);
			$$->d.var_def.type = new_array_type($4->info.value, $1);
			$$->d.var_def.name = $2;
			$$->d.var_def.is_register = 0;
			$$->d.var_def.initializer = $8;
		}
	| type IDENTIFIER '[' ']' '=' '{' expression '}' ';'
		{
			int count = 1;
			expression_node* node = $7;
			while (node->kind == EXPR_OPERATOR && node->info.op.kind == OP_COMMA) {
				count++;
				node = node->info.op.operands[0];
			}
			$$ = new_ast_node(NODE_VAR_DEF, @1.first_line);
			$$->d.var_def.type = new_array_type(count, $1);
			$$->d.var_def.name = $2;
			$$->d.var_def.is_register = 0;
			$$->d.var_def.initializer = $7;
		}
	;

func_def
	: type IDENTIFIER '(' ')' block
		{
			$$ = new_ast_node(NODE_FUNC_DEF, @1.first_line);
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
			$$ = new_ast_node(NODE_ARRAY, @1.first_line);
			$$->d.array.num = 0;
			$$->d.array.nodes = NULL;
		}
	| '{' block_elements '}'
		{ $$ = ast_chain_to_array($2, @1.first_line); }
	;

block_elements
	: statement
		{ $$ = new_chain_node($1, NULL); }
	| statement block_elements
		{ $$ = new_chain_node($1, $2); }
	;

statement
	: block
		{ $$ = $1; }
	| var_def
		{ $$ = $1; }
	| REGISTER var_def
		{
			$$ = $2;
			$$->d.var_def.is_register = 1;
		}
	| expression ';'
		{
			$$ = new_ast_node(NODE_EXPR, @1.first_line);
			$$->d.expr.expression = $1;
		}
	| ';'
		{ $$ = new_ast_node(NODE_EMPTY, @1.first_line); }
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
	fprintf(stderr, "parse error: %s at line %d\n", str, yylloc.first_line);
	return 0;
}

ast_node* build_ast(FILE* fp) {
	extern FILE* yyin;
	yyin = fp;
	yylloc.first_line = 1;
	if (yyparse()) return NULL;
	return top_ast;
}
