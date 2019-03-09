#ifndef AST_H_GUARD_C9EA5AE0_9410_4E26_A081_84B8E58D4CF1
#define AST_H_GUARD_C9EA5AE0_9410_4E26_A081_84B8E58D4CF1

#include <stdio.h>
#include <stdint.h>

typedef enum {
	NODE_ARRAY,
	NODE_VAR_DEF,
	NODE_FUNC_DEF
} node_kind;

typedef enum {
	TYPE_PRIM,
	TYPE_PTR,
	TYPE_ARRAY
} type_kind;

typedef struct type_node {
	type_kind kind;
	int size;
	int align;
	union {
		int is_signed; // TYPE_PRIM
		struct type_node* target_type; // TYPE_PTR
		struct type_node* element_type; // TYPE_ARRAY
	} info;
} type_node;

typedef enum {
	EXPR_INTEGER_LITERAL,
	EXPR_IDENTIFIER,
	EXPR_OPERATOR
} expression_type;

typedef enum {
	// 単項演算子
	OP_PARENTHESIS,
	OP_DUMMY_BINARY_START, // 二項演算子
	OP_COMMA,
	OP_DUMMY_TERNARY_START, // 三項演算子
	OP_COND
} operator_type;

typedef struct expression_node {
	expression_type kind;
	type_node* type;
	union {
		uint32_t value; // EXPR_INTEGER_LITERAL
		char* name; // EXPR_IDENTIFIER
		struct {
			operator_type kind;
			struct expression_node* operands[3];
		} op; // EXPR_OPERATOR
	} info;
} expression_node;

typedef struct ast_node {
	node_kind kind;
	union {
		struct {
			size_t num;
			struct ast_node** nodes;
		} array;
		struct {
			type_node* type;
			char* name;
			struct expression_node* initializer;
		} var_def;
		struct {
			type_node* return_type;
			char* name;
			struct ast_node* body;
		} func_def;
	} d;
} ast_node;

typedef struct ast_chain_node {
	ast_node* node;
	struct ast_chain_node* next;
} ast_chain_node;

#ifdef __cplusplus
extern "C" {
#endif

// compile15.y
ast_node* build_ast(FILE* fp);

// ast.c
ast_node* new_ast_node(node_kind kind);
ast_chain_node* new_chain_node(ast_node* element, ast_chain_node* next);
ast_node* ast_chain_to_array(ast_chain_node* chain); // もとのchainは開放する

type_node* new_prim_type(int size, int is_signed);
type_node* new_ptr_type(type_node* target_type);
type_node* new_array_type(int nelem, type_node* element_type);

expression_node* new_integer_literal(uint32_t value, int is_signed);
expression_node* new_expr_identifier(char* name);
expression_node* new_operator(operator_type op, ...);

#ifdef __cplusplus
}
#endif

#endif
