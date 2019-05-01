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
%token INC DEC SIZEOF SHL SHR LE GE EQ NEQ LAND LOR
%token MUL_A DIV_A MOD_A ADD_A SUB_A SHL_A SHR_A AND_A XOR_A OR_A
%token VOID UNSIGNED CHAR SHORT INT REGISTER
%token PRAGMA
%token GOTO RETURN
%type <type> type
%type <expression> expression
%type <node> top var_define func_define block statement control
%type <node> top_element block_element pragma_element function_define_arg
%type <node_chain> top_elements block_elements pragma_elements
%type <node_chain> function_define_args controls

/* 下に行くほど優先順位が高い */
%left ','
%right '=' MUL_A DIV_A MOD_A ADD_A SUB_A SHL_A SHR_A AND_A XOR_A OR_A
%right '?' ':'
%left LOR
%left LAND
%left '|'
%left '^'
%left '&'
%left EQ NEQ
%left '<' '>' LE GE
%left SHL SHR
%left '+' '-'
%left '*' '/' '%'
%right INC DEC ADDRESS INDIRECTION PLUS NEG '~' '!' SIZEOF CAST
%left '[' ']' '(' ')' POST_INC POST_DEC

%start top
%%
top
	: top_elements
		{ $$ = top_ast = ast_chain_to_array($1, @1.first_line); }
	;

top_elements
	: top_element
		{ $$ = new_chain_node(NULL, $1); }
	| top_elements top_element
		{ $$ = new_chain_node($1, $2); }
	;

top_element
	: var_define
	| func_define
	| control
	;

var_define
	: type IDENTIFIER ';'
		{
			$$ = new_ast_node(NODE_VAR_DEFINE, @1.first_line);
			$$->d.var_def.type = $1;
			$$->d.var_def.name = $2;
			$$->d.var_def.is_register = 0;
			$$->d.var_def.initializer = NULL;
			$$->d.var_def.info = NULL;
		}
	| type IDENTIFIER '=' expression ';'
		{
			$$ = new_ast_node(NODE_VAR_DEFINE, @1.first_line);
			$$->d.var_def.type = $1;
			$$->d.var_def.name = $2;
			$$->d.var_def.is_register = 0;
			$$->d.var_def.initializer = $4;
			$$->d.var_def.info = NULL;
		}
	| type IDENTIFIER '[' expression ']' ';'
		{
			expression_node* elem_num = constfold($4);
			if (elem_num->kind != EXPR_INTEGER_LITERAL || (int)(elem_num->info.value) <= 0) {
				yyerror("non-constant or unsupported array length");
				YYERROR;
			}
			$$ = new_ast_node(NODE_VAR_DEFINE, @1.first_line);
			$$->d.var_def.type = new_array_type(elem_num->info.value, $1);
			$$->d.var_def.name = $2;
			$$->d.var_def.is_register = 0;
			$$->d.var_def.initializer = NULL;
			$$->d.var_def.info = NULL;
		}
	| type IDENTIFIER '[' expression ']' '=' '{' expression '}' ';'
		{
			expression_node* elem_num = constfold($4);
			if (elem_num->kind != EXPR_INTEGER_LITERAL || (int)(elem_num->info.value) <= 0) {
				yyerror("non-constant or unsupported array length");
				YYERROR;
			}
			$$ = new_ast_node(NODE_VAR_DEFINE, @1.first_line);
			$$->d.var_def.type = new_array_type(elem_num->info.value, $1);
			$$->d.var_def.name = $2;
			$$->d.var_def.is_register = 0;
			$$->d.var_def.initializer = $8;
			$$->d.var_def.info = NULL;
		}
	| type IDENTIFIER '[' ']' '=' '{' expression '}' ';'
		{
			int count = 1;
			expression_node* node = $7;
			while (node->kind == EXPR_OPERATOR && node->info.op.kind == OP_COMMA) {
				count++;
				node = node->info.op.operands[0];
			}
			$$ = new_ast_node(NODE_VAR_DEFINE, @1.first_line);
			$$->d.var_def.type = new_array_type(count, $1);
			$$->d.var_def.name = $2;
			$$->d.var_def.is_register = 0;
			$$->d.var_def.initializer = $7;
			$$->d.var_def.info = NULL;
		}
	;

func_define
	: type IDENTIFIER '(' ')' block
		{
			$$ = new_ast_node(NODE_FUNC_DEFINE, @1.first_line);
			$$->d.func_def.return_type = $1;
			$$->d.func_def.name = $2;
			$$->d.func_def.arguments = NULL;
			$$->d.func_def.body = $5;
		}
	| type IDENTIFIER '(' VOID ')' block
		{
			$$ = new_ast_node(NODE_FUNC_DEFINE, @1.first_line);
			$$->d.func_def.return_type = $1;
			$$->d.func_def.name = $2;
			$$->d.func_def.arguments = new_ast_node(NODE_ARRAY, @4.first_line);
			$$->d.func_def.arguments->d.array.num = 0;
			$$->d.func_def.arguments->d.array.nodes = NULL;
			$$->d.func_def.body = $6;
		}
	| type IDENTIFIER '(' function_define_args ')' block
		{
			$$ = new_ast_node(NODE_FUNC_DEFINE, @1.first_line);
			$$->d.func_def.return_type = $1;
			$$->d.func_def.name = $2;
			$$->d.func_def.arguments = ast_chain_to_array($4, @4.first_line);
			$$->d.func_def.body = $6;
		}
	;

function_define_args
	: function_define_arg
		{ $$ = new_chain_node(NULL, $1); }
	| function_define_args ',' function_define_arg
		{ $$ = new_chain_node($1, $3); }
	;

function_define_arg
	: type IDENTIFIER
		{
			$$ = new_ast_node(NODE_ARGUMENT, @1.first_line);
			$$->d.arg.type = $1;
			$$->d.arg.name = $2;
			$$->d.arg.is_register = 0;
			$$->d.arg.pragmas = NULL;
		}
	| controls type IDENTIFIER
		{
			$$ = new_ast_node(NODE_ARGUMENT, @2.first_line);
			$$->d.arg.type = $2;
			$$->d.arg.name = $3;
			$$->d.arg.is_register = 0;
			$$->d.arg.pragmas = ast_chain_to_array($1, @1.first_line);
		}
	| REGISTER type IDENTIFIER
		{
			$$ = new_ast_node(NODE_ARGUMENT, @1.first_line);
			$$->d.arg.type = $2;
			$$->d.arg.name = $3;
			$$->d.arg.is_register = 1;
			$$->d.arg.pragmas = NULL;
		}
	| controls REGISTER type IDENTIFIER
		{
			$$ = new_ast_node(NODE_ARGUMENT, @2.first_line);
			$$->d.arg.type = $3;
			$$->d.arg.name = $4;
			$$->d.arg.is_register = 1;
			$$->d.arg.pragmas = ast_chain_to_array($1, @1.first_line);
		}
	;

controls
	: control
		{ $$ = new_chain_node(NULL, $1); }
	| controls control
		{ $$ = new_chain_node($1, $2); }
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
	| VOID
		{ $$ = new_void_type(); }
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
	: block_element
		{ $$ = new_chain_node(NULL, $1); }
	| block_elements block_element
		{ $$ = new_chain_node($1, $2); }
	;

block_element
	: statement
	| control
	;

statement
	: block
		{ $$ = $1; }
	| var_define
		{ $$ = $1; }
	| REGISTER var_define
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
	| IDENTIFIER ':' statement
		{
			$$ = new_ast_node(NODE_LABEL, @1.first_line);
			$$->d.label.name = $1;
			$$->d.label.statement = $3;
		}
	| GOTO IDENTIFIER ';'
		{
			$$ = new_ast_node(NODE_GOTO, @1.first_line);
			$$->d.go_to.label = $2;
		}
	| RETURN ';'
		{
			$$ = new_ast_node(NODE_RETURN, @1.first_line);
			$$->d.ret.ret_expression = NULL;
		}
	| RETURN expression ';'
		{
			$$ = new_ast_node(NODE_RETURN, @1.first_line);
			$$->d.ret.ret_expression = $2;
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
	| expression '[' expression ']'
		{ $$ = new_operator(OP_ARRAY_REF, $1, $3); }
	| expression '(' ')'
		{ $$ = new_operator(OP_FUNC_CALL_NOARGS, $1); }
	| expression '(' expression ')'
		{ $$ = new_operator(OP_FUNC_CALL, $1, $3); }
	| expression INC %prec POST_INC
		{ $$ = new_operator(OP_POST_INC, $1); }
	| expression DEC %prec POST_DEC
		{ $$ = new_operator(OP_POST_DEC, $1); }
	| INC expression
		{ $$ = new_operator(OP_PRE_INC, $2); }
	| DEC expression
		{ $$ = new_operator(OP_PRE_DEC, $2); }
	| '&' expression %prec ADDRESS
		{ $$ = new_operator(OP_ADDRESS, $2); }
	| '*' expression %prec INDIRECTION
		{ $$ = new_operator(OP_INDIRECTION, $2); }
	| '+' expression %prec PLUS
		{ $$ = new_operator(OP_PLUS, $2); }
	| '-' expression %prec NEG
		{ $$ = new_operator(OP_NEG, $2); }
	| '~' expression
		{ $$ = new_operator(OP_NOT, $2); }
	| '!' expression
		{ $$ = new_operator(OP_LNOT, $2); }
	| SIZEOF expression
		{ $$ = new_operator(OP_SIZEOF, $2); }
	| SIZEOF '(' type ')'
		{ $$ = new_integer_literal($3->size, 0); }
	| '(' type ')' expression %prec CAST
		{
			$$ = new_operator(OP_CAST, $4, $2);
		}
	| expression '*' expression
		{ $$ = new_operator(OP_MUL, $1, $3); }
	| expression '/' expression
		{ $$ = new_operator(OP_DIV, $1, $3); }
	| expression '%' expression
		{ $$ = new_operator(OP_MOD, $1, $3); }
	| expression '+' expression
		{ $$ = new_operator(OP_ADD, $1, $3); }
	| expression '-' expression
		{ $$ = new_operator(OP_SUB, $1, $3); }
	| expression SHL expression
		{ $$ = new_operator(OP_SHL, $1, $3); }
	| expression SHR expression
		{ $$ = new_operator(OP_SHR, $1, $3); }
	| expression '<' expression
		{ $$ = new_operator(OP_LESS, $1, $3); }
	| expression '>' expression
		{ $$ = new_operator(OP_GREATER, $1, $3); }
	| expression LE expression
		{ $$ = new_operator(OP_LESS_EQUAL, $1, $3); }
	| expression GE expression
		{ $$ = new_operator(OP_GREATER_EQUAL, $1, $3); }
	| expression EQ expression
		{ $$ = new_operator(OP_EQUAL, $1, $3); }
	| expression NEQ expression
		{ $$ = new_operator(OP_NOT_EQUAL, $1, $3); }
	| expression '&' expression
		{ $$ = new_operator(OP_AND, $1, $3); }
	| expression'^'expression
		{ $$ = new_operator(OP_XOR, $1, $3); }
	| expression '|' expression
		{ $$ = new_operator(OP_OR, $1, $3); }
	| expression LAND expression
		{ $$ = new_operator(OP_LAND, $1, $3); }
	| expression LOR expression
		{ $$ = new_operator(OP_LOR, $1, $3); }
	| expression '?' expression ':' expression
		{ $$ = new_operator(OP_COND, $1, $3, $5); }
	| expression '=' expression
		{ $$ = new_operator(OP_ASSIGN, $1, $3); }
	| expression MUL_A expression
		{ $$ = new_operator(OP_MUL_ASSIGN, $1, $3); }
	| expression DIV_A expression
		{ $$ = new_operator(OP_DIV_ASSIGN, $1, $3); }
	| expression MOD_A expression
		{ $$ = new_operator(OP_MOD_ASSIGN, $1, $3); }
	| expression ADD_A expression
		{ $$ = new_operator(OP_ADD_ASSIGN, $1, $3); }
	| expression SUB_A expression
		{ $$ = new_operator(OP_SUB_ASSIGN, $1, $3); }
	| expression SHL_A expression
		{ $$ = new_operator(OP_SHL_ASSIGN, $1, $3); }
	| expression SHR_A expression
		{ $$ = new_operator(OP_SHR_ASSIGN, $1, $3); }
	| expression AND_A expression
		{ $$ = new_operator(OP_AND_ASSIGN, $1, $3); }
	| expression XOR_A expression
		{ $$ = new_operator(OP_XOR_ASSIGN, $1, $3); }
	| expression OR_A expression
		{ $$ = new_operator(OP_OR_ASSIGN, $1, $3); }
	| expression ',' expression
		{ $$ = new_operator(OP_COMMA, $1, $3); }
	;

control
	: '#' PRAGMA '\n'
		{
			$$ = new_ast_node(NODE_PRAGMA, @1.first_line);
			$$->d.array.num = 0;
			$$->d.array.nodes = NULL;
		}
	| '#' PRAGMA pragma_elements '\n'
		{
			$$ = ast_chain_to_array($3, @1.first_line);
			$$->kind = NODE_PRAGMA;
		}
	;

pragma_elements
	: pragma_element
		{ $$ = new_chain_node(NULL, $1); }
	| pragma_elements pragma_element
		{ $$ = new_chain_node($1, $2); }
	;

pragma_element
	: INTEGER_LITERAL
		{
			$$ = new_ast_node(NODE_CONTROL_INTEGER, @1.first_line);
			$$->d.integer.value = $1;
		}
	| IDENTIFIER
		{
			$$ = new_ast_node(NODE_CONTROL_IDENTIFIER, @1.first_line);
			$$->d.identifier.name = $1;
		}
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
