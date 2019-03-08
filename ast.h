#ifndef AST_H_GUARD_C9EA5AE0_9410_4E26_A081_84B8E58D4CF1
#define AST_H_GUARD_C9EA5AE0_9410_4E26_A081_84B8E58D4CF1

#include <stdio.h>

typedef enum {
	NODE_ARRAY,
	NODE_FUNC_DEF
} node_kind;

typedef enum {
	TYPE_INT
} type_kind;

typedef struct ast_node {
	node_kind kind;
	union {
		struct {
			size_t num;
			struct ast_node** nodes;
		} array;
		struct {
			type_kind return_type;
			char* name;
			struct ast_node* body;
		} func_def;
	} d;
} ast_node;

typedef struct ast_chain_node {
	ast_node* node;
	struct ast_chain_node* next;
} ast_chain_node;

ast_node* build_ast(FILE* fp);

#endif
