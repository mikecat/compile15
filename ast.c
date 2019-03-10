#include <stdlib.h>
#include <stdarg.h>
#include "ast.h"
#include "util.h"

ast_node* new_ast_node(node_kind kind, int lineno) {
	ast_node* node = malloc_check(sizeof(ast_node));
	node->kind = kind;
	node->lineno = lineno;
	return node;
}

ast_chain_node* new_chain_node(ast_chain_node* next, ast_node* element) {
	ast_chain_node* cnode = malloc_check(sizeof(ast_chain_node));
	cnode->node = element;
	cnode->next = next;
	return cnode;
}

// もとのchainは開放する
ast_node* ast_chain_to_array(ast_chain_node* chain, int lineno) {
	size_t count = 0;
	ast_chain_node* chain_ptr = chain;
	// 要素数を求める
	while (chain_ptr != NULL) {
		count++;
		chain_ptr = chain_ptr->next;
	}
	// 要素をarrayノードに格納する
	// ついでにchainを開放する
	ast_node* array = new_ast_node(NODE_ARRAY, lineno);
	array->d.array.num = count;
	array->d.array.nodes = malloc_check(sizeof(ast_node) * count);
	chain_ptr = chain;
	for (size_t i = 0; i < count; i++) {
		array->d.array.nodes[count - 1 - i] = chain_ptr->node;
		ast_chain_node* chain_next = chain_ptr->next;
		free(chain_ptr);
		chain_ptr = chain_next;
	}
	return array;
}

type_node* new_prim_type(int size, int is_signed) {
	type_node* node = malloc_check(sizeof(type_node));
	node->kind = TYPE_INTEGER;
	node->size = size;
	node->align = size;
	node->info.is_signed = is_signed;
	return node;
}

type_node* new_ptr_type(type_node* target_type) {
	type_node* node = malloc_check(sizeof(type_node));
	node->kind = TYPE_POINTER;
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

type_node* new_void_type(void) {
	type_node* node = malloc_check(sizeof(type_node));
	node->kind = TYPE_VOID;
	node->size = 1;
	node->align = 1;
	return node;
}

type_node* integer_promotion(type_node* type) {
	if (type == NULL || type->kind != TYPE_INTEGER || type->size >= 4) return type;
	// (ランクがint以下の整数型について)
	// 全値域がintで表せる型であれば、intにする
	// そうでなければ、unsigned intにする
	return new_prim_type(4, type->size < 4 || type->info.is_signed);
}

type_node* usual_arithmetic_conversion(type_node* t1, type_node* t2) {
	type_node* t1p = integer_promotion(t1);
	type_node* t2p = integer_promotion(t2);
	if (t1p == NULL || t2p == NULL || t1p->kind != TYPE_INTEGER || t2p->kind != TYPE_INTEGER) return NULL;
	if (t1p->info.is_signed == t2p->info.is_signed) {
		// 両方符号付き or 両方符号なし → ランクが高い方に合わせる
		return t1p->size >= t2p->size ? t1p : t2p;
	} else {
		type_node *us_type, *s_type;
		if (t1p->info.is_signed) {
			us_type = t2p;
			s_type = t1p;
		} else {
			us_type = t1p;
			s_type = t2p;
		}
		if (us_type->size >= s_type->size) {
			// 符号なしのランクが符号付き以上 → 符号なしに合わせる
			return us_type;
		} else if (s_type->size > us_type->size) {
			// 符号なしの値域全てが符号付きで表せる → 符号付きに合わせる
			return s_type;
		} else {
			// 符号付きの方の符号なし版に合わせる
			return new_prim_type(s_type->size, 0);
		}
	}
}

int type_is_compatible(type_node* t1, type_node* t2) {
	if (t1 == NULL || t2 == NULL || t1->kind != t2->kind) return 0;
	switch (t1->kind) {
	case TYPE_INTEGER:
		return t1->size == t2->size && t1->info.is_signed == t2->info.is_signed;
	case TYPE_POINTER:
		return type_is_compatible(t1->info.target_type, t2->info.target_type);
	case TYPE_ARRAY:
		return t1->size == t2->size &&
			type_is_compatible(t1->info.element_type, t2->info.element_type);
	case TYPE_VOID:
		return 1;
	}
	return 0;
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

expression_node* new_operator(operator_type op, ...) {
	expression_node* node = malloc_check(sizeof(expression_node));
	va_list args;
	node->kind = EXPR_OPERATOR;
	node->type = NULL;
	node->info.op.kind = op;
	va_start(args, op);
	node->info.op.operands[0] = va_arg(args, expression_node*);
	if (op > OP_DUMMY_BINARY_START) {
		node->info.op.operands[1] = va_arg(args, expression_node*);
	}
	if (op > OP_DUMMY_TERNARY_START) {
		node->info.op.operands[2] = va_arg(args, expression_node*);
	}
	va_end(args);
	set_operator_expression_type(node);
	return node;
}

void set_operator_expression_type(expression_node* node) {
	if (node == NULL || node->kind != EXPR_OPERATOR) return;

	type_node* types[3] = {NULL, NULL, NULL};
	types[0] = node->info.op.operands[0]->type;
	if (node->info.op.kind > OP_DUMMY_BINARY_START) types[1] = node->info.op.operands[1]->type;
	if (node->info.op.kind > OP_DUMMY_TERNARY_START) types[2] = node->info.op.operands[2]->type;

	node->type = NULL;
	switch (node->info.op.kind) {
	case OP_PARENTHESIS:
		node->type = types[0];
		break;
	case OP_POST_INC:
	case OP_POST_DEC:
	case OP_PRE_INC:
	case OP_PRE_DEC:
		if (types[0] != NULL &&
		(types[0]->kind == TYPE_INTEGER || types[0]->kind == TYPE_ARRAY)) {
			node->type = types[0];
		}
		break;
	case OP_ADDRESS:
		if (types[0] != NULL) {
			node->type = new_ptr_type(types[0]);
		}
		break;
	case OP_INDIRECTION:
		if (types[0] != NULL && types[0]->kind == TYPE_POINTER) {
			node->type = types[0]->info.target_type;
		}
		break;
	case OP_PLUS:
	case OP_NEG:
	case OP_NOT:
		node->type = integer_promotion(types[0]);
		break;
	case OP_LNOT:
		node->type = new_prim_type(4, 1);
		break;
	case OP_SIZEOF:
		node->type = new_prim_type(4, 0);
		break;
	case OP_DUMMY_BINARY_START: break;
	case OP_ARRAY_REF:
		if (types[0] != NULL && types[1] != NULL &&
		((types[0]->kind == TYPE_POINTER && types[1]->kind == TYPE_INTEGER) ||
		(types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_POINTER))) {
			if (types[0]->kind == TYPE_POINTER) node->type = types[0]->info.target_type;
			else node->type = types[1]->info.target_type;
		}
		break;
	case OP_FUNC_CALL:
		// この時点では識別子の型の情報が無いので、判定不可
		node->type = NULL; break;
	case OP_MUL:
	case OP_DIV:
	case OP_MOD:
		if (types[0] != NULL && types[1] != NULL &&
		types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_INTEGER) {
			node->type = usual_arithmetic_conversion(types[0], types[1]);
		}
		break;
	case OP_ADD:
		if (types[0] != NULL && types[1] != NULL) {
			if (types[0]->kind == TYPE_INTEGER) {
				if (types[1]->kind == TYPE_INTEGER) {
					node->type = usual_arithmetic_conversion(types[0], types[1]);
				} else if (types[1]->kind == TYPE_POINTER &&
				types[1]->info.target_type->kind != TYPE_VOID) {
					node->type = types[1];
				}
			} else if (types[0]->kind == TYPE_POINTER && types[1]->kind == TYPE_INTEGER &&
			types[0]->info.target_type->kind != TYPE_VOID) {
				node->type = types[0];
			}
		}
		break;
	case OP_SUB:
		if (types[0] != NULL && types[1] != NULL) {
			if (types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_INTEGER) {
				node->type = usual_arithmetic_conversion(types[0], types[1]);
			} else if (types[0]->kind == TYPE_POINTER && types[1]->kind == TYPE_INTEGER &&
			types[0]->info.target_type->kind != TYPE_VOID) {
				node->type = types[0];
			} else if (types[0]->kind == TYPE_POINTER && types[1]->kind == TYPE_POINTER &&
			type_is_compatible(types[0]->info.target_type, types[1]->info.target_type) &&
			types[0]->info.target_type->kind != TYPE_VOID) {
				node->type = new_prim_type(4, 1);
			}
		}
		break;
	case OP_SHL:
	case OP_SHR:
		if (types[0] != NULL && types[1] != NULL &&
		types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_INTEGER) {
			node->type = integer_promotion(types[0]);
		}
		break;
	case OP_LESS:
	case OP_GREATER:
	case OP_LESS_EQUAL:
	case OP_GREATER_EQUAL:
		if (types[0] != NULL && types[1] != NULL) {
			if ((types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_INTEGER) ||
			(types[0]->kind == TYPE_POINTER && types[1]->kind == TYPE_POINTER &&
			type_is_compatible(types[0]->info.target_type, types[1]->info.target_type))) {
				node->type = new_prim_type(4, 1);
			}
		}
		break;
	case OP_EQUAL:
	case OP_NOT_EQUAL:
		if (types[0] != NULL && types[1] != NULL) {
			if ((types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_INTEGER) ||
			(types[0]->kind == TYPE_POINTER && types[1]->kind == TYPE_POINTER &&
			(type_is_compatible(types[0]->info.target_type, types[1]->info.target_type) ||
			types[0]->info.target_type->kind == TYPE_VOID ||
			types[1]->info.target_type->kind == TYPE_VOID))) {
				// 整数と整数、ポインタとポインタ
				node->type = new_prim_type(4, 1);
			} else {
				// ポインタと null pointer constantの比較かをチェックする
				expression_node* integer_node = NULL;
				if (types[0]->kind == TYPE_POINTER && types[1]->kind == TYPE_INTEGER) {
					integer_node = node->info.op.operands[1];
				} else if (types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_POINTER) {
					integer_node = node->info.op.operands[0];
				}
				if (integer_node != NULL && integer_node->kind == EXPR_INTEGER_LITERAL &&
				integer_node->info.value == 0) {
					node->type = new_prim_type(4, 1);
				}
			}
		}
		break;
	case OP_AND:
	case OP_XOR:
	case OP_OR:
		if (types[0] != NULL && types[1] != NULL &&
		types[0]->kind == TYPE_INTEGER && types[1]->kind == TYPE_INTEGER) {
			node->type = usual_arithmetic_conversion(types[0], types[1]);
		}
		break;
	case OP_LAND:
	case OP_LOR:
		node->type = new_prim_type(4, 1);
		break;
	case OP_ASSIGN:
	case OP_MUL_ASSIGN:
	case OP_DIV_ASSIGN:
	case OP_MOD_ASSIGN:
	case OP_ADD_ASSIGN:
	case OP_SUB_ASSIGN:
	case OP_SHL_ASSIGN:
	case OP_SHR_ASSIGN:
	case OP_AND_ASSIGN:
	case OP_XOR_ASSIGN:
	case OP_OR_ASSIGN:
		node->type = types[0];
		break;
	case OP_COMMA:
		node->type = types[1];
		break;
	case OP_DUMMY_TERNARY_START: break;
	case OP_COND:
		if (types[1] != NULL && types[2] != NULL) {
			if (types[1]->kind == TYPE_INTEGER && types[2]->kind == TYPE_INTEGER) {
				// 整数と整数
				node->type = usual_arithmetic_conversion(types[1], types[2]);
			} else if(types[1]->kind == TYPE_POINTER && types[2]->kind == TYPE_POINTER) {
				// ポインタとポインタ
				if (type_is_compatible(types[1]->info.target_type, types[2]->info.target_type)) {
					node->type = types[1];
				} else if (types[1]->info.target_type->kind == TYPE_VOID) {
					node->type = types[1];
				} else if (types[2]->info.target_type->kind == TYPE_VOID) {
					node->type = types[2];
				}
			} else {
				// ポインタと null pointer constantかをチェックする
				expression_node* integer_node = NULL;
				type_node* pointer_type = NULL;
				if (types[1]->kind == TYPE_POINTER && types[2]->kind == TYPE_INTEGER) {
					integer_node = node->info.op.operands[2];
					pointer_type = types[1];
				} else if (types[1]->kind == TYPE_INTEGER && types[2]->kind == TYPE_POINTER) {
					integer_node = node->info.op.operands[1];
					pointer_type = types[2];
				}
				if (integer_node != NULL && integer_node->kind == EXPR_INTEGER_LITERAL &&
				integer_node->info.value == 0) {
					node->type = pointer_type;
				}
			}
		}
		break;
	}
}
