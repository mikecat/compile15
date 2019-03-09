#include <stdlib.h>
#include "ast.h"
#include "util.h"

ast_node* new_ast_node(node_kind kind) {
	ast_node* node = malloc_check(sizeof(ast_node));
	node->kind = kind;
	return node;
}

ast_chain_node* new_chain_node(ast_node* element, ast_chain_node* next) {
	ast_chain_node* cnode = malloc_check(sizeof(ast_chain_node));
	cnode->node = element;
	cnode->next = next;
	return cnode;
}

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
	ast_node* array = new_ast_node(NODE_ARRAY);
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

type_node* new_prim_type(int size, int is_signed) {
	type_node* node = malloc_check(sizeof(type_node));
	node->kind = TYPE_PRIM;
	node->size = size;
	node->align = size;
	node->info.is_signed = is_signed;
	return node;
}

type_node* new_ptr_type(type_node* target_type) {
	type_node* node = malloc_check(sizeof(type_node));
	node->kind = TYPE_PTR;
	node->size = 4;
	node->align = 4;
	node->info.target_type = target_type;
	return node;
}

type_node* new_array_type(int nelem, type_node* element_type) {
	type_node* node = malloc_check(sizeof(type_node));
	node->kind = TYPE_ARRAY;
	node->size = nelem * element_type->size;
	node->align = element_type->align;
	node->info.element_type = element_type;
	return node;
}

expression_node* new_integer_literal(uint32_t value, int is_signed) {
	expression_node* node = malloc_check(sizeof(expression_node));
	node->kind = EXPR_INTEGER_LITERAL;
	node->type = new_prim_type(4, is_signed);
	node->info.value = value;
	return node;
}

expression_node* new_expr_identifier(char* name) {
	expression_node* node = malloc_check(sizeof(expression_node));
	node->kind = EXPR_IDENTIFIER;
	node->type = NULL;
	node->info.name = name;
	return node;
}
